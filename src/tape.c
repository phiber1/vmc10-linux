/*
 * tape.c - MC-10 cassette images: C10/CAS/K7 byte streams and WAV audio.
 */
#include "tape.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>

#define TAPE_CLOCK 894886.0
#define HALF_1 (TAPE_CLOCK / 4800.0)   /* '1' bit = one cycle of 2400 Hz */
#define HALF_0 (TAPE_CLOCK / 2400.0)   /* '0' bit = one cycle of 1200 Hz */

static bool push_half(tape_in *t, float d)
{
    if (t->n == t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 65536;
        float *p = realloc(t->half, nc * sizeof *p);
        if (!p) return false;
        t->half = p; t->cap = nc;
    }
    t->half[t->n++] = d;
    return true;
}

static void push_byte(tape_in *t, uint8_t b)
{
    for (int i = 0; i < 8; i++, b >>= 1) {
        float h = (float)((b & 1) ? HALF_1 : HALF_0);
        push_half(t, h);
        push_half(t, h);
    }
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc(sz ? (size_t)sz : 1);
    if (buf && fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); buf = NULL; }
    fclose(f);
    *len = (size_t)sz;
    return buf;
}

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static bool load_wav(tape_in *t, const uint8_t *d, size_t len, char *err, size_t errlen)
{
    if (len < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) {
        snprintf(err, errlen, "not a RIFF/WAVE file");
        return false;
    }
    unsigned fmt = 0, ch = 0, rate = 0, bits = 0;
    const uint8_t *data = NULL;
    size_t datalen = 0;
    size_t p = 12;
    while (p + 8 <= len) {
        uint32_t cl = le32(d + p + 4);
        const uint8_t *c = d + p + 8;
        size_t avail = len - (p + 8);
        if (cl > avail) cl = (uint32_t)avail;
        if (!memcmp(d + p, "fmt ", 4) && cl >= 16) {
            fmt = le16(c); ch = le16(c + 2); rate = le32(c + 4); bits = le16(c + 14);
        } else if (!memcmp(d + p, "data", 4)) {
            data = c; datalen = cl;
        }
        p += 8 + cl + (cl & 1);
    }
    if (!data || !rate || !ch || (fmt != 1 && fmt != 0xFFFE) || (bits != 8 && bits != 16)) {
        snprintf(err, errlen, "unsupported WAV (need 8/16-bit PCM)");
        return false;
    }
    size_t frame = ch * (bits / 8);
    size_t ns = datalen / frame;
    double cyc_per_sample = TAPE_CLOCK / rate;

    /* pass 1: find DC and peak */
    double mean = 0, peak = 0;
    for (size_t i = 0; i < ns; i++) {
        const uint8_t *s = data + i * frame;
        double v = bits == 8 ? (s[0] - 128) / 128.0 : (int16_t)le16(s) / 32768.0;
        mean += v;
    }
    mean = ns ? mean / ns : 0;
    for (size_t i = 0; i < ns; i++) {
        const uint8_t *s = data + i * frame;
        double v = (bits == 8 ? (s[0] - 128) / 128.0 : (int16_t)le16(s) / 32768.0) - mean;
        if (fabs(v) > peak) peak = fabs(v);
    }
    double hyst = peak * 0.08;

    /* pass 2: zero crossings with hysteresis -> half-wave lengths */
    int level = 0;
    double since = 0;
    push_half(t, 2000);
    for (size_t i = 0; i < ns; i++) {
        const uint8_t *s = data + i * frame;
        double v = (bits == 8 ? (s[0] - 128) / 128.0 : (int16_t)le16(s) / 32768.0) - mean;
        since += cyc_per_sample;
        int nl = level;
        if (level == 0 && v > hyst) nl = 1;
        else if (level == 1 && v < -hyst) nl = 0;
        if (nl != level) {
            push_half(t, (float)since);
            since = 0;
            level = nl;
        }
    }
    push_half(t, (float)since);
    return true;
}

bool tape_in_load(tape_in *t, const char *path, char *err, size_t errlen)
{
    size_t len;
    uint8_t *d = read_file(path, &len);
    if (!d) { snprintf(err, errlen, "cannot read %s", path); return false; }

    tape_in_free(t);
    bool ok;
    const char *ext = strrchr(path, '.');
    if ((ext && !strcasecmp(ext, ".wav")) || (len >= 4 && !memcmp(d, "RIFF", 4))) {
        ok = load_wav(t, d, len, err, errlen);
    } else {
        /* C10 / CAS / K7: raw byte stream as it appears on tape. */
        push_half(t, 2000);
        /* Make sure there's a usable leader in front of the first block. */
        size_t lead = 0;
        while (lead < len && d[lead] == 0x55) lead++;
        if (lead < 64)
            for (int i = 0; i < 128; i++) push_byte(t, 0x55);
        for (size_t i = 0; i < len; i++) push_byte(t, d[i]);
        for (int i = 0; i < 16; i++) push_byte(t, 0x55);
        ok = true;
    }
    free(d);
    if (!ok) { tape_in_free(t); return false; }
    t->loaded = true;
    t->idx = 0; t->into = 0;
    const char *base = strrchr(path, '/');
    snprintf(t->name, sizeof t->name, "%s", base ? base + 1 : path);
    return true;
}

void tape_in_free(tape_in *t)
{
    free(t->half);
    memset(t, 0, sizeof *t);
}

void tape_in_rewind(tape_in *t) { t->idx = 0; t->into = 0; }

int tape_in_advance(tape_in *t, double cycles)
{
    if (!t->loaded) return 0;
    t->into += cycles;
    while (t->idx < t->n && t->into >= t->half[t->idx]) {
        t->into -= t->half[t->idx];
        t->idx++;
    }
    if (t->idx >= t->n) { t->into = 0; return 0; }
    return (int)(t->idx & 1);
}

bool tape_in_done(const tape_in *t) { return !t->loaded || t->idx >= t->n; }

double tape_in_progress(const tape_in *t)
{
    return t->n ? (double)t->idx / (double)t->n : 0.0;
}

/* ---------------- output (CSAVE) ---------------- */

#define OUT_GAP        3000     /* cycles: longer pause = new byte framing */
#define OUT_THRESHOLD  290      /* cycles: high half-wave shorter => '1' */

static void out_push(tape_out *t, uint8_t b)
{
    if (t->len == t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 4096;
        uint8_t *p = realloc(t->buf, nc);
        if (!p) return;
        t->buf = p; t->cap = nc;
    }
    t->buf[t->len++] = b;
}

void tape_out_edge(tape_out *t, int level, uint64_t cycle)
{
    /* The ROM writes 1 then 0 for every bit.  The line idles high, so the
     * first "1" of a recording is not an edge: time bits from the most
     * recent write of 1 rather than from the last rising edge. */
    if (level) {
        if (cycle - t->last_edge > OUT_GAP) t->nbits = 0;
        t->rise = cycle;
        t->level = 1;
        return;
    }
    if (t->level == 0) return;
    t->level = 0;
    {
        uint64_t half = cycle - t->rise;
        int bit = half < OUT_THRESHOLD;
        t->cur = (uint8_t)((t->cur >> 1) | (bit << 7));
        if (++t->nbits == 8) {
            out_push(t, t->cur);
            t->nbits = 0;
        }
        t->active = true;
    }
    t->last_edge = cycle;
}

bool tape_out_finished(tape_out *t, uint64_t now)
{
    if (!t->active || now - t->last_edge <= (uint64_t)(TAPE_CLOCK * 1.0)) return false;
    /* Port 2 bit 0 is shared with the serial/printer line.  Only treat the
     * output as a recording if it contains a real tape block (leader byte
     * followed by the $3C sync byte); otherwise discard it. */
    for (size_t i = 0; i + 1 < t->len; i++)
        if (t->buf[i] == 0x55 && t->buf[i + 1] == 0x3C) return true;
    tape_out_clear(t);
    return false;
}

void tape_out_clear(tape_out *t)
{
    t->len = 0;
    t->nbits = 0;
    t->active = false;
}

bool tape_header_name(const uint8_t *b, size_t len, char *name, size_t nl)
{
    for (size_t i = 0; i + 12 < len; i++) {
        if (b[i] == 0x55 && b[i + 1] == 0x3C && b[i + 2] == 0x00 && b[i + 3] >= 8) {
            size_t k = 0;
            for (int j = 0; j < 8 && k + 1 < nl; j++) {
                uint8_t c = b[i + 4 + j];
                if (c == ' ' || c == 0) continue;
                name[k++] = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                            (c >= 'a' && c <= 'z') || c == '-' || c == '_' ? (char)c : '_';
            }
            name[k] = 0;
            return k > 0;
        }
    }
    return false;
}

/* ---------------- WAV writer ---------------- */

static void w32(FILE *f, uint32_t v) { uint8_t b[4] = { v, v >> 8, v >> 16, v >> 24 }; fwrite(b, 1, 4, f); }
static void w16(FILE *f, uint16_t v) { uint8_t b[2] = { v, v >> 8 }; fwrite(b, 1, 2, f); }

bool tape_bytes_to_wav(const uint8_t *buf, size_t len, const char *path)
{
    const unsigned rate = 22050;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, f);
    w32(f, 16); w16(f, 1); w16(f, 1); w32(f, rate); w32(f, rate); w16(f, 1); w16(f, 8);
    fwrite("data\0\0\0\0", 1, 8, f);
    long data_start = ftell(f);

    double t = 0, next = 0;          /* in seconds */
    int level = 0;
    /* half a second of silence, then the stream */
    for (unsigned i = 0; i < rate / 2; i++) fputc(128, f);
    for (size_t i = 0; i < len; i++) {
        uint8_t b = buf[i];
        for (int k = 0; k < 8; k++, b >>= 1) {
            double half = (b & 1) ? 1.0 / 4800 : 1.0 / 2400;
            for (int h = 0; h < 2; h++) {
                next += half;
                while (t < next) { fputc(level ? 220 : 36, f); t += 1.0 / rate; }
                level ^= 1;
            }
        }
    }
    for (unsigned i = 0; i < rate / 2; i++) fputc(128, f);

    long end = ftell(f);
    uint32_t dlen = (uint32_t)(end - data_start);
    fseek(f, 4, SEEK_SET); w32(f, (uint32_t)(end - 8));
    fseek(f, data_start - 4, SEEK_SET); w32(f, dlen);
    fclose(f);
    return true;
}

bool tape_c10_to_wav(const char *c10path, const char *wavpath)
{
    size_t len;
    uint8_t *d = read_file(c10path, &len);
    if (!d) return false;
    bool ok = tape_bytes_to_wav(d, len, wavpath);
    free(d);
    return ok;
}

/* ---------------- WAV -> cassette bytes ---------------- */

/* Decode a recording into the byte stream it carries (C10 layout).
 * Each bit is one full cycle: short (2400 Hz) = 1, long (1200 Hz) = 0.
 * Byte alignment is found per segment (segments are separated by
 * silence) by looking for a $55 leader followed by the $3C sync byte. */
bool tape_wav_to_bytes(const char *path, uint8_t **out, size_t *outlen)
{
    tape_in t;
    char err[128];
    memset(&t, 0, sizeof t);
    if (!tape_in_load(&t, path, err, sizeof err)) return false;
    const double thr = (HALF_0 + HALF_1) / 2.0;     /* half-wave threshold */

    uint8_t *res = NULL;
    size_t rn = 0, rcap = 0;
    uint8_t *bits = malloc(t.n + 1);
    size_t i = 0;
    while (bits && i < t.n) {
        /* collect one segment of bits */
        size_t nb = 0;
        while (i + 1 < t.n) {
            float a = t.half[i], b = t.half[i + 1];
            if (a > HALF_0 * 3 || b > HALF_0 * 3) { i += 2; if (nb > 64) break; nb = 0; continue; }
            int ca = a < thr, cb = b < thr;
            if (ca == cb) { bits[nb++] = (uint8_t)ca; i += 2; }
            else i += 1;                             /* resynchronise phase */
        }
        if (i + 1 >= t.n) i = t.n;
        /* find alignment: $55 then $3C */
        size_t start = (size_t)-1;
        for (size_t k = 0; k + 16 <= nb && start == (size_t)-1; k++) {
            unsigned b1 = 0, b2 = 0;
            for (int j = 0; j < 8; j++) { b1 |= (unsigned)bits[k + j] << j; b2 |= (unsigned)bits[k + 8 + j] << j; }
            if (b1 == 0x55 && b2 == 0x3C) start = k % 8;
        }
        if (start == (size_t)-1) continue;
        for (size_t k = start; k + 8 <= nb; k += 8) {
            unsigned v = 0;
            for (int j = 0; j < 8; j++) v |= (unsigned)bits[k + j] << j;
            if (rn == rcap) {
                rcap = rcap ? rcap * 2 : 4096;
                uint8_t *q = realloc(res, rcap);
                if (!q) break;
                res = q;
            }
            res[rn++] = (uint8_t)v;
        }
    }
    free(bits);
    tape_in_free(&t);
    if (!rn) { free(res); return false; }
    *out = res;
    *outlen = rn;
    return true;
}
