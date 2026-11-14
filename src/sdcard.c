/*
 * sdcard.c - emulation of the MCX32-SD's ATmega328P SD-card controller,
 * serving a host folder as the SD card.
 *
 * The host<->controller protocol was reverse engineered from the MCX BASIC
 * 3.1 BIOS (see linux/docs/MCX32-SD_protocol.md).  In short:
 *
 *   $04 status: bit0 byte waiting in $05, bit1 ready for a byte, bit2 idle,
 *               bit7 error (code in $05).  Writing $80 aborts/resyncs.
 *   Commands:   24 <letter> <name...> <trailer<$20> [...]   path commands
 *               25 <letter> <file#> [4-byte arg] [...]       handle commands
 *               2A                                           (no-op)
 *               23 ...                                       ATmega system
 *
 * Like the Emcee file server, the controller owns the file formats: LOAD
 * delivers a tokenized program image (converted from ASCII .BAS, .C10 or
 * .WAV), LOADM delivers a CoCo DECB stream, and SAVE converts back
 * according to the file name extension.
 */
#include "mc10.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

/* error codes (decimal, as BASIC reports them) */
enum { E_FC = 8, E_IO = 34, E_FM = 36, E_DN = 38, E_NE = 40, E_WP = 42, E_FN = 44,
       E_FS = 46, E_IE = 48, E_FD = 50, E_AO = 52, E_NO = 54, E_DS = 56, E_AE = 58,
       E_DF = 60, E_CR = 62 };

/* parser states */
enum { S_IDLE, S_LETTER, S_NAME, S_NAME2, S_SAVEHDR, S_DATA, S_HLETTER, S_HFILE, S_HARG };

#define ST_DATA  0x01
#define ST_READY 0x02
#define ST_IDLE  0x04
#define ST_ERR   0x80

#define BASIC_START 0x4346          /* TXTTAB of MC-10 / MCX BASIC */
#define TOKEN_TABLE 0x0E00          /* in flash bank 0 ($2E00) */

static bool trace(void)
{
    static int t = -1;
    if (t < 0) t = getenv("VMC10_TRACE_SD") != NULL;
    return t;
}

/* ------------------------------------------------------------------ */
/* output queue                                                        */

static void out_clear(sdctl *s) { s->out_len = s->out_pos = 0; }

static void out_bytes(sdctl *s, const void *p, size_t n)
{
    if (s->out_len + n > s->out_cap) {
        size_t nc = (s->out_len + n) * 2 + 1024;
        uint8_t *q = realloc(s->out, nc);
        if (!q) return;
        s->out = q; s->out_cap = nc;
    }
    memcpy(s->out + s->out_len, p, n);
    s->out_len += n;
}

static void out_byte(sdctl *s, uint8_t b) { out_bytes(s, &b, 1); }
static void out_word(sdctl *s, unsigned w) { out_byte(s, (uint8_t)(w >> 8)); out_byte(s, (uint8_t)w); }

static void fail(mc10 *m, int code)
{
    m->sd.err = code;
    if (trace()) fprintf(stderr, "sd: -> error %d\n", code);
}

/* ------------------------------------------------------------------ */
/* small buffer helpers                                                */

typedef struct { uint8_t *p; size_t n, cap; } buf;

static void bput(buf *b, const void *p, size_t n)
{
    if (b->n + n > b->cap) {
        size_t nc = (b->n + n) * 2 + 256;
        uint8_t *q = realloc(b->p, nc);
        if (!q) return;
        b->p = q; b->cap = nc;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
}
static void bbyte(buf *b, uint8_t v) { bput(b, &v, 1); }
static void bword(buf *b, unsigned w) { bbyte(b, (uint8_t)(w >> 8)); bbyte(b, (uint8_t)w); }

static bool read_whole(const char *path, buf *b)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t tmp[8192];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) bput(b, tmp, n);
    fclose(f);
    return true;
}

static bool write_whole(const char *path, const void *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(p, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

/* ------------------------------------------------------------------ */
/* paths                                                               */

static bool is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static bool exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* Resolve a card path to a normalised virtual path ("/A/B"), relative to
 * the current directory, the program directory ("$/...") or the root. */
static void virt_path(mc10 *m, const char *name, char *out, size_t outlen)
{
    sdctl *s = &m->sd;
    char tmp[2048];
    const char *base;
    if (name[0] == '/') { base = ""; }
    else if (name[0] == '$' && (name[1] == '/' || name[1] == 0)) {
        base = s->progdir[0] ? s->progdir : s->cwd;
        name += name[1] ? 2 : 1;
    } else base = s->cwd;
    snprintf(tmp, sizeof tmp, "%s/%s", base, name);

    /* normalise components */
    char *comp[128];
    int n = 0;
    for (char *t = strtok(tmp, "/"); t; t = strtok(NULL, "/")) {
        if (!strcmp(t, ".") || !*t) continue;
        if (!strcmp(t, "..")) { if (n) n--; continue; }
        if (n < 128) comp[n++] = t;
    }
    size_t k = 0;
    out[0] = 0;
    for (int i = 0; i < n && k + 2 < outlen; i++)
        k += (size_t)snprintf(out + k, outlen - k, "/%s", comp[i]);
    if (!n) snprintf(out, outlen, "/");
}

/* Map a virtual path to a host path, matching each component
 * case-insensitively against what exists on disk. */
static void host_path(mc10 *m, const char *virt, char *out, size_t outlen)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", virt);
    snprintf(out, outlen, "%s", m->cfg.sd_root);
    for (char *t = strtok(tmp, "/"); t; t = strtok(NULL, "/")) {
        char found[512] = "";
        DIR *d = opendir(out);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)))
                if (!strcasecmp(e->d_name, t)) { snprintf(found, sizeof found, "%s", e->d_name); break; }
            closedir(d);
        }
        size_t l = strlen(out);
        snprintf(out + l, outlen - l, "/%s", found[0] ? found : t);
    }
}

static bool has_ext(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(path, '.');
    return dot && (!slash || dot > slash);
}

static const char *ext_of(const char *path)
{
    return has_ext(path) ? strrchr(path, '.') : "";
}

/* Resolve a name for reading, trying default extensions when it has none. */
static bool find_file(mc10 *m, const char *name, const char *const *exts, char *out, size_t outlen)
{
    char v[1024], h[2048];
    virt_path(m, name, v, sizeof v);
    if (!has_ext(v) && exts) {
        for (int i = 0; exts[i]; i++) {
            char ve[1100];
            snprintf(ve, sizeof ve, "%s%s", v, exts[i]);
            host_path(m, ve, h, sizeof h);
            if (exists(h) && !is_dir(h)) { snprintf(out, outlen, "%s", h); return true; }
        }
    }
    host_path(m, v, h, sizeof h);
    snprintf(out, outlen, "%s", h);
    return exists(h) && !is_dir(h);
}

/* Resolve a name for writing: default extension if none, parent must exist. */
static bool create_path(mc10 *m, const char *name, const char *defext, char *out, size_t outlen)
{
    char v[1024], ve[1100];
    virt_path(m, name, v, sizeof v);
    if (!strcmp(v, "/")) return false;
    snprintf(ve, sizeof ve, "%s%s", v, (!has_ext(v) && defext) ? defext : "");
    host_path(m, ve, out, outlen);
    char parent[2048];
    snprintf(parent, sizeof parent, "%s", out);
    char *sl = strrchr(parent, '/');
    if (sl) *sl = 0;
    return is_dir(parent);
}

static void dirname_of(const char *virt, char *out, size_t outlen)
{
    snprintf(out, outlen, "%s", virt);
    char *sl = strrchr(out, '/');
    if (sl && sl != out) *sl = 0;
    else snprintf(out, outlen, "/");
}

/* ------------------------------------------------------------------ */
/* BASIC tokenizer / detokenizer (MCX BASIC 3 token table from flash)  */

/* Find the keyword table of the BASIC that is running: MCX BASIC 3 (flash
 * bank 0 at $2E00), MCX BASIC 2.1 (MCX-128 EPROM) or stock MicroColor BASIC.
 * Every table starts with "FOR" "GOTO". */
static const uint8_t *find_token_table(mc10 *m)
{
    static const uint8_t sig[] = { 'F', 'O', 'R' | 0x80, 'G', 'O', 'T', 'O' | 0x80 };
    if (m->cfg.expansion == EXP_MCX32SD) return m->mcx32.flash + TOKEN_TABLE;
    const uint8_t *img = m->rom;
    size_t len = sizeof m->rom;
    if (m->cfg.expansion == EXP_MCX128 && m->have_mcx_rom) { img = m->mcx_rom; len = sizeof m->mcx_rom; }
    for (size_t i = 0; i + sizeof sig < len; i++)
        if (!memcmp(img + i, sig, sizeof sig)) return img + i;
    return m->rom;
}

static int load_tokens(mc10 *m, const char *tok[128], char store[1024])
{
    const uint8_t *t = find_token_table(m);
    int n = 0;
    size_t k = 0;
    for (int i = 0; i < 0x300 && n < 0x78 && k < 1000; i++) {
        if ((t[i] & 0x7F) < 0x20) break;          /* end of table */
        if (!i || (t[i - 1] & 0x80)) tok[n] = store + k;
        store[k++] = (char)(t[i] & 0x7F);
        if (t[i] & 0x80) { store[k++] = 0; n++; }
    }
    return n;                       /* $80 .. $80+n-1 */
}

size_t sd_tokenize(mc10 *m, const char *text, size_t len, uint8_t *out, size_t cap, uint16_t base)
{
    const char *tok[128];
    char store[1024];
    int ntok = load_tokens(m, tok, store);
    size_t o = 0;
    size_t i = 0;
    while (i < len) {
        /* one line */
        size_t e = i;
        while (e < len && text[e] != '\n' && text[e] != '\r') e++;
        size_t p = i;
        while (p < e && (text[p] == ' ' || text[p] == '\t')) p++;
        if (p < e && isdigit((unsigned char)text[p])) {
            unsigned line = 0;
            while (p < e && isdigit((unsigned char)text[p])) line = line * 10 + (unsigned)(text[p++] - '0');
            if (p < e && text[p] == ' ') p++;
            size_t start = o;
            if (o + 4 >= cap) return 0;
            o += 2;                                   /* link, filled below */
            out[o++] = (uint8_t)(line >> 8);
            out[o++] = (uint8_t)line;
            bool quote = false, rem = false, data = false;
            while (p < e && o + 3 < cap) {
                char c = text[p];
                if (rem) { out[o++] = (uint8_t)c; p++; continue; }
                if (c == '"') { quote = !quote; out[o++] = '"'; p++; continue; }
                if (quote) { out[o++] = (uint8_t)c; p++; continue; }
                if (data) {
                    if (c == ':') data = false;
                    out[o++] = (uint8_t)c; p++; continue;
                }
                if (c == '?') { out[o++] = 0x86; p++; continue; }          /* PRINT */
                if (c == '\'') { out[o++] = ':'; out[o++] = 0xDD; rem = true; p++; continue; }
                int hit = -1;
                size_t hlen = 0;
                if (c != ' ')
                    for (int t = 0; t < ntok; t++) {
                        size_t kl = strlen(tok[t]);
                        if (kl && p + kl <= e && !strncasecmp(text + p, tok[t], kl)) { hit = t; hlen = kl; break; }
                    }
                if (hit >= 0) {
                    uint8_t code = (uint8_t)(0x80 + hit);
                    if (code == 0xC9) out[o++] = ':';                      /* ELSE */
                    out[o++] = code;
                    p += hlen;
                    if (code == 0x83) rem = true;                          /* REM */
                    if (code == 0x85) data = true;                         /* DATA */
                    continue;
                }
                out[o++] = (uint8_t)toupper((unsigned char)c);
                p++;
            }
            out[o++] = 0;
            uint16_t next = (uint16_t)(base + o);
            out[start] = (uint8_t)(next >> 8);
            out[start + 1] = (uint8_t)next;
        }
        i = e;
        while (i < len && (text[i] == '\n' || text[i] == '\r')) i++;
    }
    if (o + 2 <= cap) { out[o++] = 0; out[o++] = 0; }
    return o;
}

size_t sd_detokenize(mc10 *m, const uint8_t *img, size_t len, char *out, size_t cap)
{
    const char *tok[128];
    char store[1024];
    int ntok = load_tokens(m, tok, store);
    size_t i = 0, o = 0;
    while (i + 4 <= len && (img[i] | img[i + 1]) && o + 300 < cap) {
        unsigned line = (unsigned)(img[i + 2] << 8 | img[i + 3]);
        i += 4;
        o += (size_t)snprintf(out + o, cap - o, "%u ", line);
        bool quote = false, rem = false, data = false;
        while (i < len && img[i] && o + 16 < cap) {
            uint8_t b = img[i++];
            if (rem || quote || (data && b != ':')) {
                if (b == '"' && !rem) quote = !quote;
                out[o++] = (char)b;
                continue;
            }
            if (data) data = false;
            if (b == '"') { quote = true; out[o++] = '"'; continue; }
            if (b == ':' && i < len && (img[i] == 0xC9 || img[i] == 0xDD)) continue;
            if (b >= 0x80 && b - 0x80 < ntok) {
                const char *k = tok[b - 0x80];
                size_t kl = strlen(k);
                memcpy(out + o, k, kl);
                o += kl;
                if (b == 0x83 || b == 0xDD) rem = true;
                if (b == 0x85) data = true;
                continue;
            }
            out[o++] = (char)b;
        }
        i++;                                           /* line terminator */
        out[o++] = '\n';
    }
    return o;
}

/* ------------------------------------------------------------------ */
/* cassette images                                                     */

typedef struct {
    bool    ok;
    uint8_t type, ascii;
    uint16_t exec, load;
    buf     data;
} c10info;

static c10info parse_c10(const uint8_t *d, size_t n)
{
    c10info c;
    memset(&c, 0, sizeof c);
    bool header = false;
    for (size_t i = 0; i + 4 <= n; ) {
        if (d[i] != 0x55 || d[i + 1] != 0x3C) { i++; continue; }
        uint8_t type = d[i + 2], len = d[i + 3];
        if (i + 4 + len > n) break;
        const uint8_t *p = d + i + 4;
        if (type == 0x00 && len >= 15 && !header) {
            header = true;
            c.type = p[8]; c.ascii = p[9];
            c.exec = (uint16_t)(p[11] << 8 | p[12]);
            c.load = (uint16_t)(p[13] << 8 | p[14]);
        } else if (type == 0x01 && header) {
            bput(&c.data, p, len);
        } else if (type == 0xFF && header) {
            c.ok = true;
            break;
        }
        i += 4u + len + 1;                            /* + checksum */
    }
    if (header && c.data.n) c.ok = true;
    return c;
}

static void c10_block(buf *b, uint8_t type, const uint8_t *p, int len)
{
    uint8_t sum = (uint8_t)(type + len);
    bbyte(b, 0x55); bbyte(b, 0x3C); bbyte(b, type); bbyte(b, (uint8_t)len);
    for (int i = 0; i < len; i++) { bbyte(b, p[i]); sum = (uint8_t)(sum + p[i]); }
    bbyte(b, sum);
    bbyte(b, 0x55);
}

static void build_c10(buf *b, const char *name, uint8_t type, uint8_t ascii,
                      uint16_t exec, uint16_t load, const uint8_t *d, size_t n)
{
    uint8_t hdr[15];
    memset(hdr, ' ', 8);
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    for (int i = 0; i < 8 && base[i] && base[i] != '.'; i++) hdr[i] = (uint8_t)toupper((unsigned char)base[i]);
    hdr[8] = type; hdr[9] = ascii; hdr[10] = 0;
    hdr[11] = (uint8_t)(exec >> 8); hdr[12] = (uint8_t)exec;
    hdr[13] = (uint8_t)(load >> 8); hdr[14] = (uint8_t)load;
    for (int i = 0; i < 128; i++) bbyte(b, 0x55);
    c10_block(b, 0x00, hdr, 15);
    for (int i = 0; i < 128; i++) bbyte(b, 0x55);
    for (size_t o = 0; o < n; o += 255) c10_block(b, 0x01, d + o, (int)(n - o > 255 ? 255 : n - o));
    c10_block(b, 0xFF, NULL, 0);
}

/* read a .C10/.CAS/.K7 or .WAV into cassette info */
static c10info read_tape(const char *path)
{
    c10info c;
    memset(&c, 0, sizeof c);
    buf raw = { 0 };
    if (!strcasecmp(ext_of(path), ".wav")) {
        uint8_t *p; size_t n;
        if (tape_wav_to_bytes(path, &p, &n)) { bput(&raw, p, n); free(p); }
    } else read_whole(path, &raw);
    if (raw.n) c = parse_c10(raw.p, raw.n);
    free(raw.p);
    return c;
}

static bool is_tape_ext(const char *e)
{
    return !strcasecmp(e, ".c10") || !strcasecmp(e, ".cas") || !strcasecmp(e, ".k7") || !strcasecmp(e, ".wav");
}

/* ------------------------------------------------------------------ */
/* commands                                                            */

static const char *const ext_basic[] = { ".BAS", ".C10", ".WAV", NULL };
static const char *const ext_ml[]    = { ".BIN", ".C10", ".WAV", NULL };
static const char *const ext_array[] = { ".ARY", ".C10", ".WAV", NULL };
static const char *const ext_scr[]   = { ".SCR", NULL };
static const char *const ext_dat[]   = { ".DAT", NULL };

/* Find and convert a file for loading.  Mode 2 (LOADM) yields a CoCo DECB
 * stream; other modes yield the raw image (tokenized program for mode 0).
 * Returns 0 or an error code. */
static int load_payload(mc10 *m, const char *name, int mode, buf *out)
{
    const char *const *exts = mode == 0 ? ext_basic : mode == 2 ? ext_ml : mode == 4 ? ext_array : ext_scr;
    char path[2048];
    memset(out, 0, sizeof *out);
    if (!find_file(m, name, exts, path, sizeof path)) return E_NE;
    const char *e = ext_of(path);

    if (mode == 2) {
        if (is_tape_ext(e)) {
            c10info c = read_tape(path);
            if (!c.ok || c.type != 2) { free(c.data.p); return E_FD; }
            for (size_t o = 0; o < c.data.n; o += 0xFF00) {
                size_t l = c.data.n - o > 0xFF00 ? 0xFF00 : c.data.n - o;
                bbyte(out, 0); bword(out, (unsigned)l); bword(out, (unsigned)(c.load + o));
                bput(out, c.data.p + o, l);
            }
            bbyte(out, 0xFF); bword(out, 0); bword(out, c.exec);
            free(c.data.p);
            return 0;
        }
        if (!read_whole(path, out) || out->n < 5 || out->p[0] != 0) { free(out->p); return E_FD; }
        return 0;
    }
    if (mode == 0) {
        buf text = { 0 };
        if (is_tape_ext(e)) {
            c10info c = read_tape(path);
            if (!c.ok || c.type != 0) { free(c.data.p); return E_FD; }
            if (c.ascii) text = c.data;
            else *out = c.data;
        } else read_whole(path, &text);
        if (text.n) {
            size_t cap = text.n * 2 + 16;
            out->p = malloc(cap);
            out->n = out->p ? sd_tokenize(m, (const char *)text.p, text.n, out->p, cap, BASIC_START) : 0;
            free(text.p);
            if (out->n <= 2) { free(out->p); return E_FD; }
        }
        char v[1024];
        virt_path(m, name, v, sizeof v);
        dirname_of(v, m->sd.progdir, sizeof m->sd.progdir);
    } else if (is_tape_ext(e)) {
        c10info c = read_tape(path);
        if (!c.ok) { free(c.data.p); return E_FD; }
        *out = c.data;
    } else read_whole(path, out);
    if (out->n > 0xFFF0) { free(out->p); return E_FD; }
    return 0;
}

static void cmd_load(mc10 *m)
{
    sdctl *s = &m->sd;
    buf out;
    int err = load_payload(m, s->name, s->trailer, &out);
    if (err) { fail(m, err); return; }
    if (s->trailer != 2) out_word(s, (unsigned)out.n);
    out_bytes(s, out.p, out.n);
    free(out.p);
}

/* Convert and write a saved block of memory; the extension picks the format. */
static int save_payload(mc10 *m, const char *name, int mode, uint16_t load, uint16_t exec,
                        const uint8_t *data, size_t dlen)
{
    const char *defext = mode == 0 ? ".BAS" : mode == 2 ? ".BIN" : mode == 4 ? ".ARY" : ".SCR";
    char path[2048];
    if (!create_path(m, name, defext, path, sizeof path)) return E_NE;
    if (is_dir(path)) return E_FN;
    const char *e = ext_of(path);
    buf out = { 0 };
    bool ok;

    if (is_tape_ext(e)) {
        uint8_t type = mode == 0 ? 0 : mode == 2 ? 2 : 4;
        build_c10(&out, path, type, 0, mode == 2 ? exec : 0, mode == 2 ? load : 0, data, dlen);
        if (!strcasecmp(e, ".wav")) {
            ok = tape_bytes_to_wav(out.p, out.n, path);
            free(out.p);
            return ok ? 0 : E_WP;
        }
    } else if (mode == 0) {
        size_t cap = dlen * 8 + 1024;
        out.p = malloc(cap);
        out.n = out.p ? sd_detokenize(m, data, dlen, (char *)out.p, cap) : 0;
    } else if (mode == 2 || !strcasecmp(e, ".bin")) {
        bbyte(&out, 0); bword(&out, (unsigned)dlen); bword(&out, load);
        bput(&out, data, dlen);
        bbyte(&out, 0xFF); bword(&out, 0); bword(&out, exec);
    } else {
        bput(&out, data, dlen);
    }
    ok = write_whole(path, out.p ? out.p : (const uint8_t *)"", out.n);
    free(out.p);
    return ok ? 0 : E_WP;
}

static void cmd_save(mc10 *m)
{
    sdctl *s = &m->sd;
    uint16_t load = (uint16_t)(s->hdr[0] << 8 | s->hdr[1]);
    uint16_t exec = (uint16_t)(s->hdr[4] << 8 | s->hdr[5]);
    int err = save_payload(m, s->name, s->trailer, load, exec, s->data, s->dlen);
    if (err) fail(m, err);
}

static int cmp_entries(const void *a, const void *b)
{
    const struct { char name[256]; bool dir; } *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static bool wild_match(const char *pat, const char *s)
{
    if (!*pat) return !*s;
    if (*pat == '*') {
        for (; ; s++) { if (wild_match(pat + 1, s)) return true; if (!*s) return false; }
    }
    if (!*s) return false;
    if (*pat == '?' || toupper((unsigned char)*pat) == toupper((unsigned char)*s)) return wild_match(pat + 1, s + 1);
    return false;
}

static void cmd_dir(mc10 *m)
{
    sdctl *s = &m->sd;
    char dirpart[260], filter[260];
    const char *sl = strrchr(s->name, '/');
    if (sl) {
        snprintf(dirpart, sizeof dirpart, "%.*s", (int)(sl - s->name + (sl == s->name)), s->name);
        snprintf(filter, sizeof filter, "%s", sl[1] ? sl + 1 : "*");
    } else {
        snprintf(dirpart, sizeof dirpart, ".");
        snprintf(filter, sizeof filter, "%s", s->name[0] ? s->name : "*");
    }
    char v[1024], h[2048];
    virt_path(m, dirpart, v, sizeof v);
    host_path(m, v, h, sizeof h);
    /* "DIR NAME" where NAME is a directory lists that directory */
    if (!strchr(filter, '*') && !strchr(filter, '?')) {
        char v2[1100], h2[2048];
        snprintf(v2, sizeof v2, "%s/%s", strcmp(v, "/") ? v : "", filter);
        host_path(m, v2, h2, sizeof h2);
        if (is_dir(h2)) { snprintf(h, sizeof h, "%s", h2); snprintf(filter, sizeof filter, "*"); }
    }
    DIR *d = opendir(h);
    if (!d) { fail(m, E_NE); return; }
    struct ent { char name[256]; bool dir; uint32_t size; } *list = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (strlen(e->d_name) > 38) continue;          /* longer than the BIOS accepts */
        if (!wild_match(filter, e->d_name)) continue;
        char full[2400];
        snprintf(full, sizeof full, "%s/%s", h, e->d_name);
        struct stat st;
        if (stat(full, &st)) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            struct ent *q = realloc(list, cap * sizeof *list);
            if (!q) break;
            list = q;
        }
        snprintf(list[n].name, sizeof list[n].name, "%s", e->d_name);
        list[n].dir = S_ISDIR(st.st_mode);
        list[n].size = list[n].dir ? 0 : (uint32_t)st.st_size;
        n++;
    }
    closedir(d);
    qsort(list, n, sizeof *list, cmp_entries);
    for (size_t i = 0; i < n; i++) {
        out_byte(s, list[i].dir ? 0x10 : 0x20);
        out_word(s, list[i].size >> 16);
        out_word(s, list[i].size & 0xFFFF);
        out_bytes(s, list[i].name, strlen(list[i].name) + 1);
    }
    free(list);
}

static void cmd_chdir(mc10 *m)
{
    sdctl *s = &m->sd;
    char v[1024], h[2048];
    virt_path(m, s->name, v, sizeof v);
    host_path(m, v, h, sizeof h);
    if (!is_dir(h)) { fail(m, E_NE); return; }
    snprintf(s->cwd, sizeof s->cwd, "%s", strcmp(v, "/") ? v : "");
    snprintf(s->progdir, sizeof s->progdir, "%s", s->cwd);
}

static void cmd_kill(mc10 *m)
{
    char v[1024], h[2048];
    virt_path(m, m->sd.name, v, sizeof v);
    host_path(m, v, h, sizeof h);
    if (!exists(h)) { fail(m, E_NE); return; }
    if (is_dir(h)) {
        if (rmdir(h)) fail(m, errno == ENOTEMPTY || errno == EEXIST ? E_AO : E_WP);
    } else if (unlink(h)) fail(m, E_WP);
}

static void cmd_rename(mc10 *m)
{
    sdctl *s = &m->sd;
    char vold[1024], hold[2048], vnew[2048], hnew[2048];
    virt_path(m, s->name, vold, sizeof vold);
    host_path(m, vold, hold, sizeof hold);
    if (!exists(hold)) { fail(m, E_NE); return; }
    char target[1100];
    if (s->name2[0] == '/' || s->name2[0] == '$') snprintf(target, sizeof target, "%s", s->name2);
    else {
        char dir[1024];
        dirname_of(vold, dir, sizeof dir);
        snprintf(target, sizeof target, "%s/%s", strcmp(dir, "/") ? dir : "", s->name2);
    }
    size_t tl = strlen(target);
    if (tl && target[tl - 1] == '/') {               /* move, keeping the name */
        const char *base = strrchr(vold, '/');
        snprintf(target + tl, sizeof target - tl, "%s", base ? base + 1 : vold);
    }
    virt_path(m, target, vnew, sizeof vnew);
    host_path(m, vnew, hnew, sizeof hnew);
    if (exists(hnew) && strcasecmp(hnew, hold)) { fail(m, E_AE); return; }
    if (rename(hold, hnew)) fail(m, E_NE);
}

static void cmd_mkdir(mc10 *m)
{
    char h[2048];
    if (!create_path(m, m->sd.name, NULL, h, sizeof h)) { fail(m, E_NE); return; }
    if (exists(h)) { fail(m, E_AE); return; }
    if (mkdir(h, 0755)) fail(m, E_WP);
}

static void cmd_create(mc10 *m)               /* 'n' / 'N': new data file */
{
    char h[2048];
    if (!create_path(m, m->sd.name, ".DAT", h, sizeof h)) { fail(m, E_NE); return; }
    if (exists(h)) { fail(m, E_AE); return; }
    FILE *f = fopen(h, "wb");
    if (!f) { fail(m, E_WP); return; }
    fclose(f);
}

static void close_file(mc10 *m, int n, bool truncate)
{
    sdctl *s = &m->sd;
    FILE *f = s->fh[n];
    if (!f) return;
    if (truncate) {
        fflush(f);
        if (s->trunc_pending[n]) { if (ftruncate(fileno(f), 0)) {} }
        else if (ftruncate(fileno(f), ftell(f))) {}
    }
    fclose(f);
    s->fh[n] = NULL;
}

static void cmd_open(mc10 *m)                 /* 'o' / 'O' */
{
    sdctl *s = &m->sd;
    int n = s->trailer & 0x0F;
    if (n >= SD_MAXFILES) { fail(m, E_DN); return; }
    if (s->fh[n]) { fail(m, E_AO); return; }
    char h[2048];
    if (!find_file(m, s->name, ext_dat, h, sizeof h)) { fail(m, E_NE); return; }
    FILE *f = fopen(h, "r+b");
    if (!f) f = fopen(h, "rb");
    if (!f) { fail(m, E_WP); return; }
    s->fh[n] = f;
    s->trunc_pending[n] = true;               /* "O" erases on first write / close */
}

static void cmd_handle(mc10 *m)
{
    sdctl *s = &m->sd;
    int n = s->file & 0x0F;
    FILE *f = s->fh[n];
    switch (s->letter) {
    case 'D': close_file(m, n, false); return;
    case 'E': close_file(m, n, true); return;
    case 'F': if (f) fflush(f); return;
    }
    if (!f) { fail(m, E_NO); return; }
    switch (s->letter) {
    case 'R': case 'T': {
        size_t want = s->arg & 0xFFFF;
        uint8_t *tmp = malloc(want ? want : 1);
        size_t got = tmp ? fread(tmp, 1, want, f) : 0;
        out_bytes(s, tmp, got);
        free(tmp);
        if (got < want) fail(m, E_IE);
        return; }
    case 'W':
        if (s->trunc_pending[n]) {
            fflush(f);
            if (ftruncate(fileno(f), 0)) {}
            rewind(f);
            s->trunc_pending[n] = false;
        }
        if (fwrite(s->data, 1, s->dlen, f) != s->dlen) fail(m, E_DF);
        return;
    case 'P': {
        s->trunc_pending[n] = false;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        if ((long)s->arg > size || s->arg == 0xFFFFFFFFu) fail(m, E_IE);
        else fseek(f, (long)s->arg, SEEK_SET);
        return; }
    case 'Z':
        fflush(f);
        if (ftruncate(fileno(f), (off_t)s->arg)) fail(m, E_WP);
        return;
    default:
        fail(m, E_FC);
    }
}

static void execute_path_cmd(mc10 *m)
{
    sdctl *s = &m->sd;
    s->name[s->nlen] = 0;
    s->name2[s->n2len] = 0;
    if (trace()) fprintf(stderr, "sd: cmd 24 '%c' \"%s\" %02X\n", s->letter, s->name, s->trailer);
    switch (s->letter) {
    case 'L': cmd_load(m); break;
    case 'S': cmd_save(m); break;
    case 'D': cmd_dir(m); break;
    case 'C': cmd_chdir(m); break;
    case 'X': cmd_kill(m); break;
    case 'R': cmd_rename(m); break;
    case 'K': cmd_mkdir(m); break;
    case 'N': case 'n': cmd_create(m); break;
    case 'O': case 'o': cmd_open(m); break;
    default: fail(m, E_FC); break;
    }
}

static bool data_needed(sdctl *s, size_t n)
{
    if (n > s->dcap) {
        uint8_t *q = realloc(s->data, n);
        if (!q) return false;
        s->data = q; s->dcap = n;
    }
    s->dneed = n;
    s->dlen = 0;
    return true;
}

/* ------------------------------------------------------------------ */
/* mailbox                                                             */

uint8_t sd_status(mc10 *m)
{
    sdctl *s = &m->sd;
    if (s->out_pos < s->out_len) return ST_DATA;
    if (s->err) return ST_ERR | ST_IDLE;
    if (s->st != S_IDLE) return ST_READY;
    return ST_IDLE;
}

uint8_t sd_read(mc10 *m)
{
    sdctl *s = &m->sd;
    if (s->out_pos < s->out_len) {
        uint8_t b = s->out[s->out_pos++];
        if (s->out_pos == s->out_len) out_clear(s);
        return b;
    }
    return (uint8_t)s->err;
}

void sd_control(mc10 *m, uint8_t v)
{
    sdctl *s = &m->sd;
    if (v & 0x80) {                    /* abort / resync */
        out_clear(s);
        s->err = 0;
        s->st = S_IDLE;
    }
}

void sd_write(mc10 *m, uint8_t v)
{
    sdctl *s = &m->sd;
    switch (s->st) {
    case S_IDLE:
        out_clear(s);
        s->err = 0;
        s->nlen = s->n2len = 0;
        s->dlen = s->dneed = 0;
        if (v == 0x24) { s->prefix = v; s->st = S_LETTER; }
        else if (v == 0x25) { s->prefix = v; s->st = S_HLETTER; }
        /* $2A: no-op.  $23 (ATmega system/bootloader commands): not
         * supported; staying idle makes the BIOS report an I/O error. */
        break;
    case S_LETTER:
        s->letter = v;
        s->st = S_NAME;
        break;
    case S_NAME:
        if (v >= 0x20) { if (s->nlen < 255) s->name[s->nlen++] = (char)v; break; }
        s->trailer = v;
        if (s->letter == 'R') { s->st = S_NAME2; break; }
        if (s->letter == 'S') { s->hn = 0; s->st = S_SAVEHDR; break; }
        s->st = S_IDLE;
        execute_path_cmd(m);
        break;
    case S_NAME2:
        if (v >= 0x20) { if (s->n2len < 255) s->name2[s->n2len++] = (char)v; break; }
        s->st = S_IDLE;
        execute_path_cmd(m);
        break;
    case S_SAVEHDR:
        s->hdr[s->hn++] = v;
        if (s->hn == 6) {
            size_t len = (size_t)(s->hdr[2] << 8 | s->hdr[3]);
            if (!data_needed(s, len)) { s->st = S_IDLE; fail(m, E_IO); break; }
            if (len == 0) { s->st = S_IDLE; execute_path_cmd(m); break; }
            s->st = S_DATA;
        }
        break;
    case S_DATA:
        s->data[s->dlen++] = v;
        if (s->dlen == s->dneed) {
            s->st = S_IDLE;
            if (s->prefix == 0x24) execute_path_cmd(m);
            else cmd_handle(m);
        }
        break;
    case S_HLETTER:
        s->letter = v;
        s->st = S_HFILE;
        break;
    case S_HFILE:
        s->file = v;
        if (strchr("RWTPZ", s->letter)) { s->an = 0; s->arg = 0; s->st = S_HARG; break; }
        s->st = S_IDLE;
        if (trace()) fprintf(stderr, "sd: cmd 25 '%c' %d\n", s->letter, s->file);
        cmd_handle(m);
        break;
    case S_HARG:
        s->arg = (s->arg << 8) | v;
        if (++s->an == 4) {
            if (trace()) fprintf(stderr, "sd: cmd 25 '%c' %d %08X\n", s->letter, s->file, s->arg);
            if (s->letter == 'W') {
                size_t len = s->arg & 0xFFFF;
                if (!data_needed(s, len)) { s->st = S_IDLE; fail(m, E_IO); break; }
                if (len) { s->st = S_DATA; break; }
            }
            s->st = S_IDLE;
            cmd_handle(m);
        }
        break;
    }
}

void sd_reset(mc10 *m)
{
    sdctl *s = &m->sd;
    for (int i = 0; i < SD_MAXFILES; i++) close_file(m, i, false);
    out_clear(s);
    s->err = 0;
    s->st = S_IDLE;
    s->cwd[0] = 0;
    s->progdir[0] = 0;
}

void sd_shutdown(mc10 *m)
{
    sdctl *s = &m->sd;
    for (int i = 0; i < SD_MAXFILES; i++) close_file(m, i, false);
    free(s->out); s->out = NULL;
    free(s->data); s->data = NULL;
}

/* ================================================================== */
/* Built-in Emcee file server                                          */
/*                                                                     */
/* Implements Darren Atkinson's MCX BASIC serial filing protocol       */
/* (docs/MCX Documentation/MCX Filing Protocol.pdf) plus the 'P' print */
/* command, on the same host folder and with the same format           */
/* conversion as the MCX32-SD's SD card.  Commands arrive as bytes     */
/* decoded from the emulated 38400 baud serial line; replies are       */
/* played back onto the RxD line.                                      */
/* ================================================================== */

enum { EM_ATTN, EM_CMD, EM_HDR, EM_NAME, EM_DATA };
#define EM_BLOCK 1024

/* run a file operation with the server's own current directory */
static char em_saved_root[1024];

static void em_enter(mc10 *m, char save_cwd[1024], char save_prog[1024])
{
    memcpy(em_saved_root, m->cfg.sd_root, 1024);
    if (m->emcee.root[0]) memcpy(m->cfg.sd_root, m->emcee.root, 1024);
    memcpy(save_cwd, m->sd.cwd, 1024);
    memcpy(save_prog, m->sd.progdir, 1024);
    memcpy(m->sd.cwd, m->emcee.cwd, 1024);
    memcpy(m->sd.progdir, m->emcee.progdir, 1024);
}

static void em_leave(mc10 *m, char save_cwd[1024], char save_prog[1024])
{
    memcpy(m->emcee.cwd, m->sd.cwd, 1024);
    memcpy(m->emcee.progdir, m->sd.progdir, 1024);
    memcpy(m->sd.cwd, save_cwd, 1024);
    memcpy(m->sd.progdir, save_prog, 1024);
    memcpy(m->cfg.sd_root, em_saved_root, 1024);
}

static void em_reply(mc10 *m, const void *p, size_t n)
{
    if (trace()) fprintf(stderr, "emcee: reply %zu bytes\n", n);
    serial_rx_push(m, p, n);
}

static void em_descriptor(mc10 *m, uint16_t addr, uint16_t size, uint16_t sum)
{
    uint8_t r[6] = { (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(size >> 8), (uint8_t)size,
                     (uint8_t)(sum >> 8), (uint8_t)sum };
    em_reply(m, r, 6);
}

static void em_close(mc10 *m, int n)
{
    emfile *f = &m->emcee.f[n];
    if (f->fh) fclose(f->fh);
    free(f->data);
    free(f->sbuf);
    memset(f, 0, sizeof *f);
}

/* Describe the next unconsumed block ('L' reply, 'N'); EOF has size 0. */
static void em_next(mc10 *m, int n)
{
    emfile *f = &m->emcee.f[n];
    if (!f->used || f->out) { em_descriptor(m, 0, 0, E_NO); return; }
    if (f->next >= f->nblk) { em_descriptor(m, f->exec, 0, 0); return; }
    f->cur = f->next;
    unsigned sum = 0;
    for (size_t i = 0; i < f->blen[f->cur]; i++) sum += f->data[f->boff[f->cur] + i];
    em_descriptor(m, f->baddr[f->cur], (uint16_t)f->blen[f->cur], (uint16_t)sum);
}

static void em_add_block(emfile *f, size_t off, size_t len, uint16_t addr)
{
    while (len && f->nblk < 512) {
        size_t l = len > EM_BLOCK ? EM_BLOCK : len;
        f->boff[f->nblk] = off; f->blen[f->nblk] = l; f->baddr[f->nblk] = addr;
        f->nblk++;
        off += l; len -= l; addr = (uint16_t)(addr + l);
    }
}

static void em_load(mc10 *m, int n, const char *name, int mode)
{
    em_close(m, n);
    emfile *f = &m->emcee.f[n];
    buf p;
    int err = load_payload(m, name, mode, &p);
    if (err) { em_descriptor(m, 0, 0, (uint16_t)err); return; }
    f->used = true;
    f->mode = mode;
    f->data = p.p;
    f->len = p.n;
    if (mode == 2) {                                  /* DECB -> addressed blocks */
        size_t i = 0;
        while (i + 5 <= p.n) {
            uint8_t t = p.p[i];
            unsigned len = (unsigned)(p.p[i + 1] << 8 | p.p[i + 2]);
            uint16_t adr = (uint16_t)(p.p[i + 3] << 8 | p.p[i + 4]);
            i += 5;
            if (t) { f->exec = adr; break; }
            if (i + len > p.n) break;
            em_add_block(f, i, len, adr);
            i += len;
        }
    } else em_add_block(f, 0, p.n, 0);
    em_next(m, n);
}

static void em_dir_free(emceesrv *e) { free(e->dir); e->dir = NULL; e->ndir = e->diridx = 0; }

static void em_dir_reply(mc10 *m, int err)
{
    emceesrv *e = &m->emcee;
    uint8_t r[2] = { (uint8_t)err, 0 };
    if (!err && e->diridx < e->ndir) r[1] = (uint8_t)strlen(e->dir[e->diridx]);
    em_reply(m, r, 2);
}

/* Fill the listing with files and subdirectories (dirs = false; directory
 * names get a trailing "/" so they can be told apart and navigated), or
 * with directories only (dirs = true, for DIR LIST). */
static int em_list(mc10 *m, const char *arg, bool dirs)
{
    emceesrv *e = &m->emcee;
    em_dir_free(e);
    char dirpart[260], filter[260];
    const char *sl = strrchr(arg, '/');
    if (sl) {
        snprintf(dirpart, sizeof dirpart, "%.*s", (int)(sl - arg + (sl == arg)), arg);
        snprintf(filter, sizeof filter, "%s", sl[1] ? sl + 1 : "*");
    } else {
        snprintf(dirpart, sizeof dirpart, ".");
        snprintf(filter, sizeof filter, "%s", arg[0] ? arg : "*");
    }
    char v[1024], h[2048];
    virt_path(m, dirpart, v, sizeof v);
    host_path(m, v, h, sizeof h);
    DIR *d = opendir(h);
    if (!d) return E_NE;
    struct dirent *de;
    int cap = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.' || strlen(de->d_name) > 38) continue;
        char full[2400];
        snprintf(full, sizeof full, "%s/%s", h, de->d_name);
        bool isd = is_dir(full);
        if ((dirs && !isd) || !wild_match(filter, de->d_name)) continue;
        if (!dirs && isd && strlen(de->d_name) > 37) continue;
        if (e->ndir == cap) {
            cap = cap ? cap * 2 : 64;
            char (*q)[40] = realloc(e->dir, (size_t)cap * sizeof *q);
            if (!q) break;
            e->dir = q;
        }
        snprintf(e->dir[e->ndir], 40, "%s%s", de->d_name, (!dirs && isd) ? "/" : "");
        for (char *c = e->dir[e->ndir]; *c; c++) *c = (char)toupper((unsigned char)*c);
        e->ndir++;
    }
    closedir(d);
    qsort(e->dir, (size_t)e->ndir, sizeof *e->dir, (int (*)(const void *, const void *))strcmp);
    return 0;
}

static void em_print(mc10 *m, uint8_t ch)
{
    if (!m->printer && m->cfg.printer_path[0]) {
        m->printer = fopen(m->cfg.printer_path, "a");
        if (m->printer) mc10_status(m, "Printer output -> %s", m->cfg.printer_path);
    }
    if (!m->printer) return;
    if (ch == 0x0D) fputc('\n', m->printer);
    else if (ch == 0x0A && m->prn_last_char == 0x0D) ;
    else if (ch >= 0x20 || ch == '\t' || ch == '\f') fputc(ch, m->printer);
    fflush(m->printer);
    m->prn_last_char = ch;
}

static void em_execute(mc10 *m)
{
    emceesrv *e = &m->emcee;
    uint8_t *h = e->hdr;
    e->name[e->nn] = 0;
    if (trace()) fprintf(stderr, "emcee: cmd '%c' \"%s\"\n", e->cmd, e->name);
    switch (e->cmd) {
    case 'L': em_load(m, 0, e->name, h[0]); break;
    case 'N': em_next(m, h[0] & 15); break;
    case 'G': {
        emfile *f = &e->f[h[0] & 15];
        if (f->used && !f->out && f->cur < f->nblk) {
            em_reply(m, f->data + f->boff[f->cur], f->blen[f->cur]);
            f->next = f->cur + 1;
        }
        break; }
    case 'S': {
        em_close(m, 0);
        emfile *f = &e->f[0];
        char path[2048];
        const char *defext = h[0] == 0 ? ".BAS" : h[0] == 2 ? ".BIN" : h[0] == 4 ? ".ARY" : ".SCR";
        uint8_t st = create_path(m, e->name, defext, path, sizeof path) ? 0 : E_NE;
        if (!st) {
            f->used = f->out = true;
            f->mode = h[0];
            f->sexec = (uint16_t)(h[2] << 8 | h[3]);
            f->ssize = (uint16_t)(h[4] << 8 | h[5]);
            snprintf(f->name, sizeof f->name, "%s", e->name);
        }
        em_reply(m, &st, 1);
        break; }
    case 'W': case 'w': {
        int n = h[0] & 15;
        emfile *f = &e->f[n];
        unsigned sum = 0;
        for (size_t i = 0; i < e->dn; i++) sum += e->data[i];
        uint8_t r[2] = { (uint8_t)(sum >> 8), (uint8_t)sum };
        if (!f->used || !f->out) { em_reply(m, r, 2); break; }
        if (f->fh) {                                  /* data file */
            if (e->cmd == 'w' && f->lastw) fseek(f->fh, -f->lastw, SEEK_CUR);
            if (e->dn) fwrite(e->data, 1, e->dn, f->fh);
            f->lastw = (long)e->dn;
            if (!e->dn) em_close(m, n);
        } else if (e->dn) {                           /* SAVE data */
            if (e->cmd == 'w' && f->lastw) f->slen -= (size_t)f->lastw;
            if (f->slen + e->dn > f->scap) {
                f->scap = (f->slen + e->dn) * 2;
                uint8_t *q = realloc(f->sbuf, f->scap);
                if (q) f->sbuf = q;
            }
            if (f->sbuf) { memcpy(f->sbuf + f->slen, e->data, e->dn); f->slen += e->dn; }
            f->lastw = (long)e->dn;
        } else {                                      /* SAVE complete */
            uint16_t load = f->mode == 2 ? f->ssize : BASIC_START;
            save_payload(m, f->name, f->mode, load, f->sexec, f->sbuf, f->slen);
            em_close(m, n);
        }
        em_reply(m, r, 2);
        break; }
    case 'O': {
        int n = h[0] & 15, mode = h[0] >> 6;
        em_close(m, n);
        emfile *f = &e->f[n];
        uint8_t st = 0;
        char path[2048];
        if (mode == 1) {                              /* input */
            buf p = { 0 };
            if (!find_file(m, e->name, ext_dat, path, sizeof path) || !read_whole(path, &p)) st = E_NE;
            else {
                f->used = true;
                f->data = p.p; f->len = p.n;
                for (size_t o = 0; o < p.n; o += 256) em_add_block(f, o, p.n - o > 256 ? 256 : p.n - o, 0);
            }
        } else if (!create_path(m, e->name, ".DAT", path, sizeof path)) st = E_NE;
        else if (!(f->fh = fopen(path, mode == 3 ? "ab" : "wb"))) st = E_WP;
        else f->used = f->out = true;
        em_reply(m, &st, 1);
        break; }
    case 'F':
        if (h[0] == 0) em_dir_reply(m, em_list(m, e->name, false));
        else { e->diridx++; em_dir_reply(m, 0); }
        break;
    case 'D':
        if (h[0] == 0) em_dir_reply(m, em_list(m, "", true));
        else { e->diridx++; em_dir_reply(m, 0); }
        break;
    case '$': {
        char nm[256];
        memset(nm, ' ', sizeof nm);
        if (e->diridx < e->ndir) memcpy(nm, e->dir[e->diridx], strlen(e->dir[e->diridx]));
        em_reply(m, nm, h[0]);
        break; }
    case 'C': {
        char v[1024], hp[2048];
        virt_path(m, e->name, v, sizeof v);
        host_path(m, v, hp, sizeof hp);
        uint8_t st = is_dir(hp) ? 0 : E_NE;
        if (!st) {
            snprintf(m->sd.cwd, sizeof m->sd.cwd, "%s", strcmp(v, "/") ? v : "");
            snprintf(m->sd.progdir, sizeof m->sd.progdir, "%s", m->sd.cwd);
        }
        em_reply(m, &st, 1);
        break; }
    case 'P': em_print(m, h[0]); break;
    }
}

/* header bytes per command, and where the name length / data size live */
static int em_hdr_len(uint8_t c)
{
    switch (c) {
    case 'L': return 2;   /* mode, nameLen */
    case 'G': case 'N': case '$': case 'P': return 1;
    case 'S': return 6;   /* mode, nameLen, exec, size */
    case 'W': case 'w': return 3;   /* file#, size */
    case 'O': case 'F': case 'D': case 'C': return 2;
    default: return -1;
    }
}

void emcee_byte(mc10 *m, uint8_t b)
{
    emceesrv *e = &m->emcee;
    char sc[1024], sp[1024];
    switch (e->st) {
    case EM_ATTN:
        if (b == '!') e->st = EM_CMD;
        return;
    case EM_CMD:
        e->cmd = b;
        e->hneed = em_hdr_len(b);
        e->hn = e->nn = 0;
        e->dn = 0;
        e->st = e->hneed < 0 ? EM_ATTN : EM_HDR;
        return;
    case EM_HDR:
        e->hdr[e->hn++] = b;
        if (e->hn < e->hneed) return;
        e->nneed = 0;
        e->dneed = 0;
        if (e->cmd == 'L' || e->cmd == 'S' || e->cmd == 'O' || e->cmd == 'F' || e->cmd == 'C')
            e->nneed = e->hdr[1];
        if (e->cmd == 'W' || e->cmd == 'w') e->dneed = (size_t)(e->hdr[1] << 8 | e->hdr[2]);
        if (e->nneed) { e->st = EM_NAME; return; }
        if (e->dneed) {
            if (e->dneed > e->dcap) {
                uint8_t *q = realloc(e->data, e->dneed);
                if (!q) { e->st = EM_ATTN; return; }
                e->data = q; e->dcap = e->dneed;
            }
            e->st = EM_DATA; return;
        }
        break;
    case EM_NAME:
        if (e->nn < 255) e->name[e->nn++] = (char)b;
        if (e->nn < e->nneed) return;
        break;
    case EM_DATA:
        e->data[e->dn++] = b;
        if (e->dn < e->dneed) return;
        break;
    }
    e->st = EM_ATTN;
    em_enter(m, sc, sp);
    em_execute(m);
    em_leave(m, sc, sp);
}

void emcee_shutdown(mc10 *m)
{
    for (int i = 0; i < 16; i++) em_close(m, i);
    em_dir_free(&m->emcee);
    free(m->emcee.data);
    m->emcee.data = NULL;
}
