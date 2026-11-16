/*
 * mc10.c - TRS-80 MC-10 machine emulation: memory map, keyboard matrix,
 * MC6847 video, 1-bit sound, cassette and printer.
 *
 * Memory map (matches the address handler tables in VMC10.exe):
 *   $0000-$001F  6803 internal registers
 *   $0080-$00FF  6803 internal RAM
 *   $4000-$4FFF  4K RAM (video memory at $4000; mirrored to $7FFF if no expander)
 *   $4000-$5FFF  8K RAM with the internal 8K mod (Kitsz / Snider piggyback);
 *                the VDG then addresses 8K, so CG6/RG6 show a full 6K screen
 *   $5000-$8FFF  Tandy 16K RAM expansion (optional)
 *   (the MCX-128 expansion replaces most of this map: see mcx128.c)
 *   $BF00-$BFFF  write: VDG mode / sound latch, read: keyboard rows
 *   $C000-$FFFF  8K BASIC ROM (mirrored at $C000)
 *
 * $BFFF write bits: 7 sound, 6 CSS, 5 A/G, 4 GM0, 3 GM1, 2 GM2 (+INT/EXT)
 * Port 1 ($02): keyboard column strobe (active low)
 * Port 2 ($03): bit0 cassette/printer out, bit1 keyboard row 6 in
 *               (CONTROL/BREAK/SHIFT), bit2 printer busy, bit4 cassette in
 */
#include "mc10.h"
#include "font6847.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

/* ROM entry points (MC-10 BASIC 1.0) used for fast typing and printing */
#define ROM_KEYSCAN   0xF883
#define ROM_PRN_BIT   0xFA0C

#define UNCONNECTED   0xFF

static uint8_t keyboard_rows(mc10 *m, bool row6)
{
    uint8_t sel = (uint8_t)(m->cpu.ddr1 & ~m->cpu.port1_out); /* driven low */
    uint8_t rows = 0;
    for (int c = 0; c < 8; c++)
        if (sel & (1 << c)) rows |= m->keys[c] | m->joy_keys[c];
    return row6 ? (rows & 0x40) : (rows & 0x3F);
}

uint8_t mc10_base_read(mc10 *m, uint16_t a)
{
    if (a >= 0xC000) return m->rom[a & 0x1FFF];
    if (a >= 0x4000 && a < m->int_ram_end) return m->mem[a];
    if (a >= 0x5000 && a < 0x9000) {
        if (m->cfg.expansion == EXP_16K) return m->mem[a];
        if (!m->cfg.internal8k && a < 0x8000) return m->mem[0x4000 | (a & 0x0FFF)];
        return UNCONNECTED;
    }
    if (a >= 0xBF00 && a < 0xC000) {
        m->kbd_read_frame = m->frame_count;
        return (uint8_t)(~keyboard_rows(m, false));
    }
    if (a >= 0x80 && a < 0x100) return m->mem[a];
    return UNCONNECTED;
}

static void audio_run(mc10 *m, int cycles);

void mc10_base_write(mc10 *m, uint16_t a, uint8_t v)
{
    if (a >= 0x4000 && a < m->int_ram_end) { m->mem[a] = v; return; }
    if (a >= 0x5000 && a < 0x9000) {
        if (m->cfg.expansion == EXP_16K) m->mem[a] = v;
        /* without the expander $5000-$7FFF mirrors the 4K for reads only
         * (VMC10 uses a separate write handler here; BASIC then sizes
         * memory correctly as 4K) */
        return;
    }
    if (a >= 0xBF00 && a < 0xC000) {
        m->vdg = v;
        m->speaker = v >> 7;
        return;
    }
    if (a >= 0x80 && a < 0x100) { m->mem[a] = v; return; }
}

static uint8_t bus_read(void *ctx, uint16_t a)
{
    mc10 *m = ctx;
    if (m->cfg.expansion == EXP_MCX128) return mcx128_read(m, a);
    if (m->cfg.expansion == EXP_MCX32SD) return mcx32sd_read(m, a);
    return mc10_base_read(m, a);
}

static void bus_write(void *ctx, uint16_t a, uint8_t v)
{
    mc10 *m = ctx;
    if (m->cfg.expansion == EXP_MCX128) mcx128_write(m, a, v);
    else if (m->cfg.expansion == EXP_MCX32SD) mcx32sd_write(m, a, v);
    else mc10_base_write(m, a, v);
}

/* Is a MicroColor-BASIC-style keyscan routine mapped at $F883 right now?
 * It starts with JSR to the keyscan RAM hook: $42A9 in stock BASIC, $01A9
 * in MCX BASIC 2.1 (system variables moved down by $4100) and $02A9 in
 * MCX BASIC 3 (moved down by $4000). */
static bool keyscan_present(mc10 *m)
{
    uint8_t b0 = bus_read(m, ROM_KEYSCAN), b1 = bus_read(m, ROM_KEYSCAN + 1),
            b2 = bus_read(m, ROM_KEYSCAN + 2);
    return b0 == 0xBD && (b1 == 0x42 || b1 == 0x01 || b1 == 0x02) && b2 == 0xA9;
}

static bool printer_present(mc10 *m)
{
    return bus_read(m, ROM_PRN_BIT) == 0xD7 && bus_read(m, ROM_PRN_BIT + 1) == 0x03;
}

bool mc10_fast_typing_active(mc10 *m)
{
    return m->cfg.fast_typing && keyscan_present(m);
}

static uint8_t port_read(void *ctx, int port)
{
    mc10 *m = ctx;
    if (port == 0) return 0xFF;

    uint64_t now = m->cpu.cycles;
    uint64_t delta = now - m->last_port2_read;
    m->last_port2_read = now;
    int tape = 0;
    if (m->tin.loaded) {
        /* "Motor control": the tape only moves while software polls it. */
        tape = tape_in_advance(&m->tin, delta < 20000 ? (double)delta : 0.0);
    }
    uint8_t v = 0xE0 | 0x08 | 0x01;           /* mode bits, RS232 idle */
    if (!keyboard_rows(m, true)) v |= 0x02;   /* row 6 active low */
    /* bit 2: RS-232 RxD.  With nothing connected it reads 0, which the
     * ROM's printer routine takes as "printer ready". */
    if (m->serial.enabled && serial_rxd(m, now)) v |= 0x04;
    if (tape) v |= 0x10;
    return v;
}

static void printer_bit(mc10 *m, int bit)
{
    switch (m->prn_state) {
    case 0:                                   /* waiting for start bit */
        if (!bit) { m->prn_state = 1; m->prn_bits = 0; m->prn_byte = 0; }
        break;
    case 1:                                   /* data bits, LSB first */
        m->prn_byte |= bit << m->prn_bits;
        if (++m->prn_bits == 8) m->prn_state = 2;
        break;
    case 2: {                                 /* stop bit */
        m->prn_state = 0;
        int ch = m->prn_byte;
        if (!m->printer && m->cfg.printer_path[0]) {
            m->printer = fopen(m->cfg.printer_path, "a");
            if (m->printer) mc10_status(m, "Printer output -> %s", m->cfg.printer_path);
        }
        if (m->printer) {
            if (ch == 0x0D) fputc('\n', m->printer);
            else if (ch == 0x0A && m->prn_last_char == 0x0D) ;
            else if (ch >= 0x20 || ch == '\t' || ch == '\f') fputc(ch, m->printer);
            fflush(m->printer);
        }
        m->prn_last_char = ch;
        break; }
    }
}

static void port_write(void *ctx, int port, uint8_t v, uint8_t ddr)
{
    mc10 *m = ctx;
    if (port != 1) return;
    int bit0 = (ddr & 1) ? (v & 1) : 1;
    if (m->cpu.ppc == ROM_PRN_BIT && printer_present(m)) {
        printer_bit(m, bit0);
        return;
    }
    serial_txd(m, bit0, m->cpu.cycles);
    tape_out_edge(&m->tout, bit0, m->cpu.cycles);
}

/* ------------------------------------------------------------------ */

void mc10_status(mc10 *m, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->status, sizeof m->status, fmt, ap);
    va_end(ap);
    m->status_new = true;
    fprintf(stderr, "vmc10: %s\n", m->status);
}

static const uint32_t default_palette[C_NUMCOLORS] = {
    0x28E028, /* green      40,224, 40 */
    0xF0F070, /* yellow    240,240,112 */
    0x2020D8, /* blue       32, 32,216 */
    0xA82020, /* red       168, 32, 32 */
    0xF0F0F0, /* buff      240,240,240 */
    0x28A8A8, /* cyan       40,168,168 */
    0xD361FA, /* magenta   211, 97,250 */
    0xF08828, /* orange    240,136, 40 */
    0x101010, /* black */
    0x106010, /* dark green  16, 96, 16 */
    0x803010, /* dark orange 128,48, 16 */
    0x3078D8, /* artifact blue    48,120,216: composite "cyan", leaning blue */
    0xD88850, /* artifact orange 216,136, 80: composite orange, washed out */
};

void mc10_default_palette(uint32_t pal[C_NUMCOLORS])
{
    memcpy(pal, default_palette, sizeof default_palette);
}

bool mc10_init(mc10 *m, const mc10_config *cfg)
{
    memset(m, 0, sizeof *m);
    m->cfg = *cfg;
    if (!m->cfg.palette[C_GREEN] && !m->cfg.palette[C_BLACK])
        memcpy(m->cfg.palette, default_palette, sizeof default_palette);
    memcpy(m->chr, font6847, sizeof m->chr);
    m->cpu.ctx = m;
    m->cpu.read = bus_read;
    m->cpu.write = bus_write;
    m->cpu.port_read = port_read;
    m->cpu.port_write = port_write;
    m6803_init(&m->cpu);
    m->serial.fd = -1;
    m->serial.tx_last = 1;
    m->tout.level = 1;
    m->int_ram_end = m->cfg.internal8k ? 0x6000 : 0x5000;
    m->vram_mask   = m->cfg.internal8k ? 0x1FFF : 0x0FFF;
    return true;
}

static bool read_exact(const char *path, uint8_t *dst, size_t want, size_t *got)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    *got = fread(dst, 1, want, f);
    fclose(f);
    return *got > 0;
}

bool mc10_load_rom(mc10 *m, const char *path)
{
    uint8_t tmp[0x2000];
    size_t n;
    if (!read_exact(path, tmp, sizeof tmp, &n)) return false;
    if (n == 0x1000) memcpy(tmp + 0x1000, tmp, 0x1000);      /* 4K ROM: mirror */
    else if (n != 0x2000) return false;
    memcpy(m->rom, tmp, sizeof tmp);
    m->stock_rom = m->rom[0x1883] == 0xBD && m->rom[0x1884] == 0x42 &&
                   m->rom[0x1885] == 0xA9 && m->rom[0x1A0C] == 0xD7 &&
                   m->rom[0x1A0D] == 0x03;
    return true;
}

bool mc10_load_mcx_rom(mc10 *m, const char *path)
{
    size_t n;
    if (!read_exact(path, m->mcx_rom, sizeof m->mcx_rom, &n) || n != sizeof m->mcx_rom) return false;
    m->have_mcx_rom = true;
    return true;
}

bool mc10_load_chr(mc10 *m, const char *path)
{
    uint8_t tmp[768];
    size_t n;
    if (!read_exact(path, tmp, sizeof tmp, &n) || n != 768) return false;
    memcpy(m->chr, tmp, 768);
    return true;
}

bool mc10_load_chrx(mc10 *m, const char *path)
{
    size_t n;
    memset(m->chrx, 0, sizeof m->chrx);
    if (!read_exact(path, m->chrx, sizeof m->chrx, &n)) return false;
    if (n < sizeof m->chrx) memcpy(m->chrx + n, m->chrx, sizeof m->chrx - n);
    m->have_chrx = true;
    return true;
}

void mc10_reset(mc10 *m, bool hard)
{
    if (hard) {
        /* power-up pattern: zeros show as a screen full of inverse '@' */
        memset(m->mem, 0, sizeof m->mem);
        m->vdg = 0;
        m->typeq_len = m->typeq_pos = 0;
    }
    if (m->cfg.expansion == EXP_MCX128) mcx128_reset(m, hard);
    if (m->cfg.expansion == EXP_MCX32SD) mcx32sd_reset(m, hard);
    m6803_reset(&m->cpu);
    m->line = 0;
    m->line_cycle = 0;
}

/* ---------------- keyboard ---------------- */

void mc10_set_key(mc10 *m, int key, bool down)
{
    if (key < 0 || key >= K_COUNT) return;
    int row = key / 8, col = key % 8;
    if (down) m->keys[col] |= (uint8_t)(1 << row);
    else      m->keys[col] &= (uint8_t)~(1 << row);
}

void mc10_release_all_keys(mc10 *m)
{
    memset(m->keys, 0, sizeof m->keys);
    memset(m->joy_keys, 0, sizeof m->joy_keys);
}

/* Map an ASCII character to the MC-10 key combination that produces it. */
bool mc10_char_to_keys(int ch, int *key, bool *shift, bool *ctrl)
{
    static const char shifted[] = "\0!\"#$%&'()*+<=>?";   /* SHIFT + 0..9 : ; , - . / */
    *shift = *ctrl = false;
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch == '@') { *key = K_AT; return true; }
    if (ch >= 'A' && ch <= 'Z') { *key = K_A + (ch - 'A'); return true; }
    if (ch >= '0' && ch <= '9') { *key = K_0 + (ch - '0'); return true; }
    switch (ch) {
    case ':': *key = K_COLON; return true;
    case ';': *key = K_SEMI; return true;
    case ',': *key = K_COMMA; return true;
    case '-': *key = K_MINUS; return true;
    case '.': *key = K_PERIOD; return true;
    case '/': *key = K_SLASH; return true;
    case ' ': *key = K_SPACE; return true;
    case '\r': case '\n': *key = K_ENTER; return true;
    case 8:   *key = K_A; *ctrl = true; return true;     /* CONTROL-A: backspace */
    case '^': *key = K_W; *ctrl = true; return true;
    }
    for (int i = 1; i < 16; i++)
        if (shifted[i] == ch) { *key = K_0 + i; *shift = true; return true; }
    return false;
}

/* ---------------- typing queue ---------------- */

void mc10_type_text(mc10 *m, const char *text, size_t len)
{
    if (m->typeq_pos == m->typeq_len) {
        m->typeq_pos = m->typeq_len = 0;
        m->type_trap_frame = m->frame_count;
    }
    if (m->typeq_len + len > m->typeq_cap) {
        size_t nc = (m->typeq_len + len) * 2 + 256;
        uint8_t *p = realloc(m->typeq, nc);
        if (!p) return;
        m->typeq = p; m->typeq_cap = nc;
    }
    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)text[i];
        if (c == '\r') { if (i + 1 < len && text[i + 1] == '\n') continue; c = '\n'; }
        if (c == '\n') c = 0x0D;
        else if (c == '\t') c = ' ';
        else if (c >= 'a' && c <= 'z' && !m->cfg.lower_as_inverse) c -= 32;
        else if (c < 0x20 && c != 0x08 && c != 0x03) continue;
        m->typeq[m->typeq_len++] = c;
    }
}

bool mc10_typing(mc10 *m) { return m->typeq_pos < m->typeq_len; }

/* ROM keyscan trap: deliver the next queued character directly. */
static bool type_trap(mc10 *m)
{
    m6803 *c = &m->cpu;
    uint8_t ch = m->typeq[m->typeq_pos++];
    m->type_trap_frame = m->frame_count;
    c->a = ch;
    c->cc &= ~(CC_N | CC_Z | CC_V);
    if (ch & 0x80) c->cc |= CC_N;
    if (!ch) c->cc |= CC_Z;
    uint16_t hi = m6803_rd(c, (uint16_t)(c->sp + 1));
    uint16_t lo = m6803_rd(c, (uint16_t)(c->sp + 2));
    c->sp += 2;
    c->pc = (uint16_t)((hi << 8) | lo);
    c->cycles += 40;
    return true;
}

/* Fallback for non-stock ROMs: press keys in the matrix for a few frames. */
static void type_matrix_frame(mc10 *m)
{
    if (m->type_timer > 0) { m->type_timer--; return; }
    memset(m->joy_keys, 0, sizeof m->joy_keys);
    if (m->type_phase == 1) {          /* key was down: now release */
        m->type_phase = 0;
        m->type_timer = (m->typeq_pos > 0 && m->typeq[m->typeq_pos - 1] == 0x0D) ? 20 : 2;
        return;
    }
    while (mc10_typing(m)) {
        int key; bool sh, ct;
        uint8_t ch = m->typeq[m->typeq_pos++];
        if (!mc10_char_to_keys(ch == 0x0D ? '\n' : ch, &key, &sh, &ct)) continue;
        m->joy_keys[key % 8] |= (uint8_t)(1 << (key / 8));
        if (sh) m->joy_keys[K_SHIFT % 8] |= 1 << 6;
        if (ct) m->joy_keys[K_CONTROL % 8] |= 1 << 6;
        m->type_phase = 1;
        m->type_timer = 2;
        return;
    }
}

/* ---------------- video ---------------- */

static const struct { uint8_t bpr, rows, bpp; } gmodes[8] = {
    { 16,  64, 2 },   /* CG1  64x64  4 colour */
    { 16,  64, 1 },   /* RG1 128x64  2 colour */
    { 32,  64, 2 },   /* CG2 128x64  4 colour */
    { 16,  96, 1 },   /* RG2 128x96  2 colour */
    { 32,  96, 2 },   /* CG3 128x96  4 colour */
    { 16, 192, 1 },   /* RG3 128x192 2 colour */
    { 32, 192, 2 },   /* CG6 128x192 4 colour */
    { 32, 192, 1 },   /* RG6 256x192 2 colour */
};

static void render_line(mc10 *m, int line)
{
    if (line >= FB_H) return;
    uint32_t *row = m->fb[line];
    const uint32_t *pal = m->cfg.palette;
    uint8_t vdg = m->vdg;
    bool css = vdg & 0x40, ag = vdg & 0x20, intext = vdg & 0x04;  /* GM2 = INT/EXT */
    uint32_t border = ag ? pal[css ? C_BUFF : C_GREEN] : pal[C_BLACK];

    int y = line - FB_BORDER_Y;
    if (y < 0 || y >= 192) {
        for (int x = 0; x < FB_W; x++) row[x] = border;
        return;
    }
    for (int x = 0; x < FB_BORDER_X; x++) row[x] = row[FB_W - 1 - x] = border;
    uint32_t *px = row + FB_BORDER_X;

    if (!ag) {
        int crow = y / 12, gl = y % 12;
        uint32_t fg = pal[css ? C_ORANGE : C_GREEN];
        uint32_t bg = pal[css ? C_DKORANGE : C_DKGREEN];
        for (int col = 0; col < 32; col++) {
            uint8_t b = m->mem[0x4000 + crow * 32 + col];
            uint32_t *p = px + col * 8;
            if (b & 0x80) {
                uint32_t on, off = pal[C_BLACK];
                int left, right;
                if (intext) {       /* semigraphics 6 */
                    on = pal[((b >> 6) & 3) + (css ? 4 : 0)];
                    int er = gl / 4;
                    left = b & (0x20 >> (er * 2));
                    right = b & (0x10 >> (er * 2));
                } else {            /* semigraphics 4 */
                    on = pal[(b >> 4) & 7];
                    left = b & (gl < 6 ? 0x08 : 0x02);
                    right = b & (gl < 6 ? 0x04 : 0x01);
                }
                for (int i = 0; i < 4; i++) { p[i] = left ? on : off; p[i + 4] = right ? on : off; }
            } else {
                uint8_t g;
                if (intext && m->have_chrx) g = m->chrx[((b & 0x7F) * 16 + gl) & 0xFFF];
                else {
                    g = m->chr[(b & 0x3F) * 12 + gl];
                    if (b & 0x40) g ^= 0xFF;
                }
                for (int i = 0; i < 8; i++) p[i] = (g & (0x80 >> i)) ? fg : bg;
            }
        }
        return;
    }

    /* GM0 is bit 4, GM1 bit 3, GM2 bit 2 (GM2 doubles as INT/EXT) */
    int mode = ((vdg >> 4) & 1) | ((vdg >> 2) & 2) | (vdg & 4);
    int bpr = gmodes[mode].bpr, bpp = gmodes[mode].bpp;
    int dl = y * gmodes[mode].rows / 192;
    unsigned base = (unsigned)dl * bpr;
    int ppb = 8 / bpp;                       /* pixels per byte */
    int w = 256 / (bpr * ppb);               /* screen pixels per pixel */
    int x = 0;
    if (mode == 7 && css && !m->cfg.no_artifact) {
        /* NTSC artifact colours: on a composite monitor, RG6 with CSS=1
         * is seen as 128x192 in four colours.  Each pair of pixels is one
         * colour: 00 black, 11 white, and 10 / 01 orange or blue (often
         * called cyan; the exact hue depends on the TV).  Which of the two
         * gets which depends on the VDG's clock phase after reset on a
         * real machine; here it is selectable. */
        uint32_t c10 = pal[m->cfg.artifact_phase ? C_ART_BLUE : C_ART_ORANGE];
        uint32_t c01 = pal[m->cfg.artifact_phase ? C_ART_ORANGE : C_ART_BLUE];
        for (int i = 0; i < bpr; i++) {
            uint8_t b = m->mem[0x4000 + ((base + i) & m->vram_mask)];
            for (int k = 0; k < 4; k++) {
                uint32_t c;
                switch ((b >> (6 - k * 2)) & 3) {
                case 0:  c = pal[C_BLACK]; break;
                case 1:  c = c01; break;
                case 2:  c = c10; break;
                default: c = pal[C_BUFF]; break;
                }
                px[x++] = c;
                px[x++] = c;
            }
        }
        return;
    }
    for (int i = 0; i < bpr; i++) {
        uint8_t b = m->mem[0x4000 + ((base + i) & m->vram_mask)];
        for (int k = 0; k < ppb; k++) {
            uint32_t c;
            if (bpp == 2) c = pal[((b >> (6 - k * 2)) & 3) + (css ? 4 : 0)];
            else c = (b & (0x80 >> k)) ? pal[css ? C_BUFF : C_GREEN] : pal[C_BLACK];
            for (int j = 0; j < w; j++) px[x++] = c;
        }
    }
}

void mc10_render_line_for_test(mc10 *m, int line) { render_line(m, line); }

/* ---------------- audio ---------------- */

static void audio_run(mc10 *m, int cycles)
{
    const double cps = (double)MC10_CLOCK / AUDIO_RATE;
    double level = m->speaker ? 1.0 : -1.0;
    if (m->aud_left <= 0) m->aud_left = cps;
    double rem = cycles;
    while (rem > 1e-9) {
        double take = rem < m->aud_left ? rem : m->aud_left;
        m->aud_acc += level * take;
        m->aud_left -= take;
        rem -= take;
        if (m->aud_left <= 1e-9) {
            double s = m->aud_acc / cps;
            m->aud_acc = 0;
            m->aud_left += cps;
            /* gentle low-pass (the real speaker is cap-coupled) + DC block */
            m->aud_lp += (s - m->aud_lp) * 0.35;
            double hp = m->aud_lp - m->aud_hp_in + 0.995 * m->aud_hp_out;
            m->aud_hp_in = m->aud_lp;
            m->aud_hp_out = hp;
            double v = hp * (m->cfg.sound ? m->cfg.volume : 0) / 100.0 * 12000.0;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            if (m->audio_len < AUDIO_MAX_FRAME) m->audio[m->audio_len++] = (int16_t)v;
        }
    }
}

/* ---------------- frame ---------------- */

static void save_recording(mc10 *m)
{
    char name[64], path[1400];
    if (!tape_header_name(m->tout.buf, m->tout.len, name, sizeof name))
        snprintf(name, sizeof name, "CSAVE");
    const char *dir = m->cfg.tape_out_dir[0] ? m->cfg.tape_out_dir : ".";
    snprintf(path, sizeof path, "%s/%s.C10", dir, name);
    struct stat st;
    for (int i = 1; stat(path, &st) == 0 && i < 1000; i++)
        snprintf(path, sizeof path, "%s/%s-%d.C10", dir, name, i);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(m->tout.buf, 1, m->tout.len, f);
        fclose(f);
        mc10_status(m, "CSAVE: wrote %zu bytes to %s", m->tout.len, path);
    } else {
        mc10_status(m, "CSAVE: cannot write %s", path);
    }
    tape_out_clear(&m->tout);
}

void mc10_run_frame(mc10 *m)
{
    m6803 *c = &m->cpu;
    m->audio_len = 0;
    m->stop_frame = false;
    bool trap_ok = m->cfg.fast_typing;

    while (m->line < MC10_LINES) {
        if (m->nbreak) {
            bool hit = false;
            for (int i = 0; i < m->nbreak; i++) if (m->breakpoints[i] == c->pc) hit = true;
            if (hit && !m->bp_skip) { m->break_hit = true; return; }
            m->bp_skip = false;
        }
        int n;
        /* While a key is held in the matrix (fallback typing), let the real
         * keyscan run so the key is seen exactly once, as on hardware. */
        if (trap_ok && c->pc == ROM_KEYSCAN && mc10_typing(m) && !m->type_phase &&
            keyscan_present(m)) {
            uint64_t before = c->cycles;
            type_trap(m);
            n = (int)(c->cycles - before);
        } else {
            n = m6803_step(c);
        }
        audio_run(m, n);
        m->line_cycle += n;
        while (m->line_cycle >= MC10_LINE_CYCLES) {
            m->line_cycle -= MC10_LINE_CYCLES;
            render_line(m, m->line);
            m->line++;
        }
        if (m->stop_frame) return;
    }
    m->line = 0;
    m->frame_count++;

    /* Typing goes through the ROM keyscan hook when software calls it.
     * Otherwise (custom keyboard routines, boot menus, non-standard ROMs)
     * press the keys in the matrix instead. */
    /* fall back only if something else is scanning the keyboard matrix */
    bool hook_idle = m->frame_count - m->type_trap_frame > 6 &&
                     m->kbd_read_frame + 1 >= m->frame_count;
    if (m->type_phase || (mc10_typing(m) && (!mc10_fast_typing_active(m) || hook_idle)))
        type_matrix_frame(m);

    if (tape_out_finished(&m->tout, c->cycles)) save_recording(m);
    serial_poll(m);
}

bool mc10_tape_busy(mc10 *m)
{
    return m->tin.loaded && !tape_in_done(&m->tin) &&
           m->cpu.cycles - m->last_port2_read < 20000;
}

void mc10_shutdown(mc10 *m)
{
    if (m->tout.len) save_recording(m);
    if (m->cfg.expansion == EXP_MCX32SD) { mcx32sd_save_flash(m); sd_shutdown(m); }
    if (m->printer) fclose(m->printer);
    serial_shutdown(m);
    emcee_shutdown(m);
    tape_in_free(&m->tin);
    free(m->tout.buf);
    free(m->typeq);
}

/* ---------------- binary helpers ---------------- */

bool mc10_load_binary(mc10 *m, const char *path, uint16_t addr)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    int ch;
    uint32_t a = addr;
    while ((ch = fgetc(f)) != EOF && a <= 0xFFFF) m6803_wr(&m->cpu, (uint16_t)a++, (uint8_t)ch);
    fclose(f);
    mc10_status(m, "Loaded %s at $%04X-$%04X", path, addr, (unsigned)(a - 1));
    return true;
}

static void put_block(FILE *f, uint8_t type, const uint8_t *data, int len)
{
    uint8_t sum = type + (uint8_t)len;
    fputc(0x55, f); fputc(0x3C, f); fputc(type, f); fputc(len, f);
    for (int i = 0; i < len; i++) { fputc(data[i], f); sum += data[i]; }
    fputc(sum, f);
    fputc(0x55, f);
}

bool mc10_csavem(mc10 *m, const char *path, const char *name,
                 uint16_t start, uint16_t end, uint16_t exec)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint8_t hdr[15];
    memset(hdr, ' ', 8);
    for (int i = 0; i < 8 && name[i]; i++) hdr[i] = (uint8_t)toupper((unsigned char)name[i]);
    hdr[8] = 2; hdr[9] = 0; hdr[10] = 0;              /* ML program, binary, no gaps */
    hdr[11] = exec >> 8; hdr[12] = exec & 0xFF;
    hdr[13] = start >> 8; hdr[14] = start & 0xFF;
    for (int i = 0; i < 128; i++) fputc(0x55, f);
    put_block(f, 0x00, hdr, 15);
    for (int i = 0; i < 128; i++) fputc(0x55, f);
    uint32_t a = start;
    while (a <= end) {
        uint8_t buf[255];
        int n = 0;
        while (n < 255 && a <= end) buf[n++] = m6803_peek(&m->cpu, (uint16_t)a++);
        put_block(f, 0x01, buf, n);
    }
    put_block(f, 0xFF, NULL, 0);
    fclose(f);
    mc10_status(m, "CSAVEM: $%04X-$%04X exec $%04X -> %s", start, end, exec, path);
    return true;
}
