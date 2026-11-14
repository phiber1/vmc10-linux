/*
 * tape.h - MC-10 cassette handling.
 *
 * Input tapes (C10/CAS/K7 byte images, or WAV recordings) are converted into
 * a list of half-wave durations measured in CPU cycles.  The emulated
 * cassette input bit is then just "which half-wave are we in".
 *
 * Output (CSAVE) is decoded from the timing of writes to port 2 bit 0 back
 * into bytes, and stored as a C10 image.
 */
#ifndef TAPE_H
#define TAPE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    float   *half;        /* half-wave durations in CPU cycles */
    size_t   n, cap;
    size_t   idx;         /* current half-wave */
    double   into;        /* cycles elapsed inside current half-wave */
    bool     loaded;
    char     name[512];
} tape_in;

typedef struct {
    int       level;
    uint64_t  rise;            /* cycle of last 0->1 transition */
    uint64_t  last_edge;
    int       nbits;
    uint8_t   cur;
    uint8_t  *buf;
    size_t    len, cap;
    bool      active;
    bool      saw_eof_block;
} tape_out;

bool   tape_in_load(tape_in *t, const char *path, char *err, size_t errlen);
void   tape_in_free(tape_in *t);
void   tape_in_rewind(tape_in *t);
int    tape_in_advance(tape_in *t, double cycles);   /* returns level 0/1 */
bool   tape_in_done(const tape_in *t);
double tape_in_progress(const tape_in *t);

void   tape_out_edge(tape_out *t, int level, uint64_t cycle);
/* call regularly: returns true when a recording has finished and the
 * caller should save t->buf/t->len (then call tape_out_clear). */
bool   tape_out_finished(tape_out *t, uint64_t now);
void   tape_out_clear(tape_out *t);
/* extract tape filename from the header block of a C10 image */
bool   tape_header_name(const uint8_t *buf, size_t len, char *name, size_t namelen);

/* write a byte image as an 8-bit mono WAV file (C10 -> WAV) */
bool   tape_bytes_to_wav(const uint8_t *buf, size_t len, const char *path);
bool   tape_c10_to_wav(const char *c10path, const char *wavpath);
/* decode a WAV recording into its cassette byte stream (malloc'd) */
bool   tape_wav_to_bytes(const char *path, uint8_t **out, size_t *outlen);

#endif
