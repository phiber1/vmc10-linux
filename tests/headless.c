/*
 * headless.c - scriptable test driver for the MC-10 core (no SDL).
 *
 * usage: headless ROM [commands...]
 *   frames:N        run N frames
 *   break:N         run N frames with the BREAK key held
 *   type:TEXT       queue text for typing (\n = ENTER)
 *   tape:FILE       insert cassette image
 *   until-tape      run until tape is finished (max 3000 frames)
 *   screen          print the text screen
 *   ppm:FILE        save framebuffer as PPM
 *   ram:N           set RAM size (4 or 20) - must come first, before boot
 *   prn:FILE        printer output file
 *   out:DIR         CSAVE output directory
 *   audio:FILE      dump raw s16le audio of following frames
 *   (see the source for the debugging commands: until:, until-illegal, bp:,
 *    mem:, sum:, poke:, csavem:, flash:, serial:, vdg:)
 */
#include "../src/mc10.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mc10 M;
static FILE *aud;

static void run(int n)
{
    for (int i = 0; i < n; i++) {
        mc10_run_frame(&M);
        if (aud) fwrite(M.audio, 2, (size_t)M.audio_len, aud);
    }
}

static void screen(void)
{
    printf("+--------------------------------+\n");
    for (int r = 0; r < 16; r++) {
        putchar('|');
        for (int c = 0; c < 32; c++) {
            uint8_t b = M.mem[0x4000 + r * 32 + c];
            char ch;
            if (b & 0x80) ch = '#';
            else {
                int code = b & 0x3F;
                ch = (char)(code < 0x20 ? code + 0x40 : code);
                if (!(b & 0x40) && ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32); /* non-inverse = lowercase */
            }
            putchar(ch);
        }
        printf("|\n");
    }
    printf("+--------------------------------+\n");
}

static void ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
    for (int y = 0; y < FB_H; y++)
        for (int x = 0; x < FB_W; x++) {
            uint32_t p = M.fb[y][x];
            fputc(p >> 16, f); fputc((p >> 8) & 0xFF, f); fputc(p & 0xFF, f);
        }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: headless ROM cmds...\n"); return 1; }
    mc10_config cfg = { .expansion = EXP_16K, .volume = 50, .sound = true, .fast_typing = true,
                        .no_remap = true };   /* same default as the frontend */
    snprintf(cfg.printer_path, sizeof cfg.printer_path, "LPRINT.TXT");
    snprintf(cfg.tape_out_dir, sizeof cfg.tape_out_dir, ".");
    int ai = 2;
    if (ai < argc && !strncmp(argv[ai], "ram:", 4)) cfg.expansion = atoi(argv[ai++] + 4) >= 16 ? EXP_16K : EXP_NONE;
    const char *mcxrom = NULL;
    if (ai < argc && !strncmp(argv[ai], "mcx128:", 7)) { cfg.expansion = EXP_MCX128; mcxrom = argv[ai++] + 7; }
    const char *mcx32 = NULL;
    if (ai < argc && !strncmp(argv[ai], "mcx32sd:", 8)) { cfg.expansion = EXP_MCX32SD; mcx32 = argv[ai++] + 8; }
    if (ai < argc && !strcmp(argv[ai], "slowtype")) { cfg.fast_typing = false; ai++; }
    if (ai < argc && !strcmp(argv[ai], "8k")) { cfg.internal8k = true; ai++; }
    if (ai < argc && !strncmp(argv[ai], "sd:", 3)) snprintf(cfg.sd_root, sizeof cfg.sd_root, "%s", argv[ai++] + 3);
    mc10_init(&M, &cfg);
    if (!mc10_load_rom(&M, argv[1])) { fprintf(stderr, "bad rom\n"); return 1; }
    if (mcxrom && !mc10_load_mcx_rom(&M, mcxrom)) { fprintf(stderr, "bad mcx rom\n"); return 1; }
    if (mcx32 && !mcx32sd_load_flash(&M, mcx32)) { fprintf(stderr, "bad mcx32sd flash\n"); return 1; }
    M.mcx32.flash_path[0] = 0;   /* tests never write the flash image back */
    printf("stock ROM: %s\n", M.stock_rom ? "yes" : "no");
    mc10_reset(&M, true);

    for (int i = ai; i < argc; i++) {
        char *a = argv[i];
        if (!strncmp(a, "frames:", 7)) run(atoi(a + 7));
        else if (!strncmp(a, "break:", 6)) {      /* break:N - hold BREAK for N frames */
            M.keys[K_BREAK % 8] |= (uint8_t)(1 << (K_BREAK / 8));
            run(atoi(a + 6));
            M.keys[K_BREAK % 8] &= (uint8_t)~(1 << (K_BREAK / 8));
        }
        else if (!strncmp(a, "type:", 5)) {
            char buf[4096]; size_t k = 0;
            for (char *p = a + 5; *p && k < sizeof buf - 1; p++) {
                if (p[0] == '\\' && p[1] == 'n') { buf[k++] = '\n'; p++; }
                else buf[k++] = *p;
            }
            mc10_type_text(&M, buf, k);
        }
        else if (!strncmp(a, "typefile:", 9)) {
            FILE *f = fopen(a + 9, "rb");
            if (f) { char buf[65536]; size_t k = fread(buf, 1, sizeof buf, f); fclose(f); mc10_type_text(&M, buf, k); }
        }
        else if (!strncmp(a, "tape:", 5)) {
            char err[256];
            if (!tape_in_load(&M.tin, a + 5, err, sizeof err)) printf("tape error: %s\n", err);
        }
        else if (!strcmp(a, "until-tape")) {
            int f = 0;
            while (!tape_in_done(&M.tin) && f < 30000) { run(1); f++; }
            printf("tape finished after %d frames (%.1f s emulated)\n", f, f / 59.94);
        }
        else if (!strcmp(a, "until-typed")) {
            int f = 0;
            while (mc10_typing(&M) && f < 20000) { run(1); f++; }
            printf("typing finished after %d frames\n", f);
        }
        else if (!strcmp(a, "until-illegal")) {
            /* single-step until an undefined opcode runs; print the last 60 instructions */
            uint16_t ring[60]; int rn = 0; long steps = 0;
            uint32_t start = M.cpu.illegal_count;
            while (M.cpu.illegal_count == start && steps < 200000000L) {
                ring[rn++ % 60] = M.cpu.pc;
                m6803_step(&M.cpu); steps++;
            }
            for (int k = rn > 60 ? rn - 60 : 0; k < rn; k++) {
                char dis[64]; uint16_t pc = ring[k % 60];
                m6803_disasm(&M.cpu, pc, dis, sizeof dis);
                printf("  %04X  %s\n", pc, dis);
            }
            printf("-> illegal %02X after %ld steps; SP=%04X bank=%02X romsel=%02X\n",
                   M.cpu.last_illegal, steps, M.cpu.sp, M.mcx_bank, M.mcx_romsel);
        }
        else if (!strncmp(a, "until:", 6)) {
            /* until:ADDR[,SKIP] - single-step until PC==ADDR (after SKIP hits),
             * then print the last 40 instructions executed */
            unsigned target = 0, skip = 0;
            sscanf(a + 6, "%x,%u", &target, &skip);
            uint16_t ring[40]; int rn = 0;
            long steps = 0;
            for (;;) {
                if (M.cpu.pc == target && skip-- == 0) break;
                ring[rn++ % 40] = M.cpu.pc;
                m6803_step(&M.cpu);
                if (++steps > 200000000L) { printf("until: not reached\n"); break; }
            }
            int start = rn > 40 ? rn - 40 : 0;
            for (int k = start; k < rn; k++) {
                char dis[64]; uint16_t pc = ring[k % 40];
                m6803_disasm(&M.cpu, pc, dis, sizeof dis);
                printf("  %04X  %s\n", pc, dis);
            }
            printf("-> reached %04X after %ld steps  A=%02X B=%02X X=%04X SP=%04X\n",
                   M.cpu.pc, steps, M.cpu.a, M.cpu.b, M.cpu.x, M.cpu.sp);
        }
        else if (!strncmp(a, "bp:", 3)) {         /* bp:ADDR - run frames until PC hits ADDR */
            unsigned ad = 0;
            sscanf(a + 3, "%x", &ad);
            M.breakpoints[0] = (uint16_t)ad; M.nbreak = 1; M.break_hit = false;
            int f = 0;
            while (!M.break_hit && f < 20000) { run(1); f++; }
            M.nbreak = 0;
            printf("bp %04X %s after %d frames\n", ad, M.break_hit ? "hit" : "NOT hit", f);
            M.break_hit = false; M.bp_skip = true;
        }
        else if (!strncmp(a, "serial:", 7)) serial_init(&M, a + 7);
        else if (!strncmp(a, "artifact:", 9)) M.cfg.no_artifact = !atoi(a + 9);
        else if (!strncmp(a, "artphase:", 9)) M.cfg.artifact_phase = atoi(a + 9) ? 1 : 0;
        else if (!strncmp(a, "remap:", 6)) M.cfg.no_remap = !atoi(a + 6);
        else if (!strncmp(a, "dbgport:", 8)) {    /* dbgport:ADDR - log writes to ADDR */
            M.cfg.debug_on = true;
            M.cfg.debug_port = (uint16_t)strtoul(a + 8, NULL, 16);
        }
        else if (!strncmp(a, "load:", 5)) {       /* load:FILE,ADDR - raw binary into memory */
            char file[1024]; unsigned ad = 0;
            if (sscanf(a + 5, "%1023[^,],%x", file, &ad) == 2 && !mc10_load_binary(&M, file, (uint16_t)ad))
                printf("load: cannot read %s\n", file);
        }
        else if (!strncmp(a, "pc:", 3)) M.cpu.pc = (uint16_t)strtoul(a + 3, NULL, 16);  /* jump */
        else if (!strncmp(a, "poke:", 5)) {       /* poke:ADDR,HEXBYTES */
            unsigned ad = 0; char hex[2048] = "";
            sscanf(a + 5, "%x,%2047s", &ad, hex);
            for (char *h = hex; h[0] && h[1]; h += 2) {
                unsigned v; sscanf(h, "%2x", &v);
                m6803_wr(&M.cpu, (uint16_t)ad++, (uint8_t)v);
            }
        }
        else if (!strncmp(a, "csavem:", 7)) {     /* csavem:FILE,NAME,START,END,EXEC (hex) */
            char file[1024], name[16]; unsigned st, en, ex;
            if (sscanf(a + 7, "%1023[^,],%15[^,],%x,%x,%x", file, name, &st, &en, &ex) == 5)
                mc10_csavem(&M, file, name, (uint16_t)st, (uint16_t)en, (uint16_t)ex);
        }
        else if (!strncmp(a, "c10wav:", 7)) {     /* c10wav:IN,OUT */
            char in[1024], outp[1024];
            if (sscanf(a + 7, "%1023[^,],%1023s", in, outp) == 2) tape_c10_to_wav(in, outp);
        }
        else if (!strncmp(a, "sum:", 4)) {        /* sum:ADDR,LEN checksum via the CPU bus */
            unsigned ad = 0, n = 0;
            sscanf(a + 4, "%x,%u", &ad, &n);
            unsigned long sum = 0, first_diff_dummy = 0;
            for (unsigned k = 0; k < n; k++) sum = sum * 31 + m6803_peek(&M.cpu, (uint16_t)(ad + k));
            (void)first_diff_dummy;
            printf("sum %04X+%u = %08lX\n", ad, n, sum & 0xFFFFFFFFul);
        }
        else if (!strncmp(a, "mem:", 4)) {        /* mem:ADDR[,N] via the CPU bus */
            unsigned ad = 0, n = 16;
            sscanf(a + 4, "%x,%u", &ad, &n);
            printf("mem %04X:", ad);
            for (unsigned k = 0; k < n; k++) printf(" %02X", m6803_peek(&M.cpu, (uint16_t)(ad + k)));
            printf("\n");
        }
        else if (!strncmp(a, "flash:", 6)) {      /* flash:OFFSET[,N] hex dump */
            unsigned off = 0, n = 32;
            sscanf(a + 6, "%x,%u", &off, &n);
            printf("flash %05X:", off);
            for (unsigned k = 0; k < n && off + k < sizeof M.mcx32.flash; k++) printf(" %02X", M.mcx32.flash[off + k]);
            printf("\n");
        }
        else if (!strcmp(a, "screen")) screen();
        else if (!strncmp(a, "vdg:", 4)) {
            /* write a VDG mode and fill 6K of video RAM with a test pattern:
             * a frame, a diagonal, and a band marking the bottom third */
            M.vdg = (uint8_t)strtoul(a + 4, NULL, 16);
            for (int y = 0; y < 192; y++)
                for (int xb = 0; xb < 32; xb++) {
                    uint8_t b = 0;
                    if (y == 0 || y == 191) b = 0xFF;
                    if (xb == 0) b |= 0x80;
                    if (xb == 31) b |= 0x01;
                    if (xb == (y * 32 / 192)) b |= (uint8_t)(0x80 >> ((y * 256 / 192) & 7));
                    if (y >= 128 && y < 136) b = 0xAA;
                    M.mem[0x4000 + y * 32 + xb] = b;
                }
            /* render one frame without letting BASIC run over the pattern */
            for (int l = 0; l < MC10_LINES; l++) {
                mc10_render_line_for_test(&M, l);
            }
        }
        else if (!strncmp(a, "ppm:", 4)) ppm(a + 4);
        else if (!strncmp(a, "prn:", 4)) snprintf(M.cfg.printer_path, sizeof M.cfg.printer_path, "%s", a + 4);
        else if (!strncmp(a, "out:", 4)) snprintf(M.cfg.tape_out_dir, sizeof M.cfg.tape_out_dir, "%s", a + 4);
        else if (!strncmp(a, "audio:", 6)) aud = fopen(a + 6, "wb");
        else if (!strcmp(a, "regs")) printf("VDG=%02X PC=%04X A=%02X B=%02X X=%04X SP=%04X CC=%02X illegal=%u last=%02X\n",
                M.vdg, M.cpu.pc, M.cpu.a, M.cpu.b, M.cpu.x, M.cpu.sp, M.cpu.cc, M.cpu.illegal_count, M.cpu.last_illegal);
        else { fprintf(stderr, "unknown command %s\n", a); return 1; }
    }
    if (aud) fclose(aud);
    mc10_shutdown(&M);
    return 0;
}
