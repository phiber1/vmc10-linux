/*
 * mc10.h - TRS-80 MC-10 / Matra Alice machine emulation.
 */
#ifndef MC10_H
#define MC10_H

#include "m6803.h"
#include "tape.h"
#include <stdio.h>

#define MC10_CLOCK        894886      /* 3.579545 MHz / 4 */
#define MC10_LINE_CYCLES  57
#define MC10_LINES        262
#define MC10_FRAME_CYCLES (MC10_LINE_CYCLES * MC10_LINES)

#define FB_W 320                      /* 256 active + 2*32 border */
#define FB_H 240                      /* 192 active + 2*24 border */
#define FB_BORDER_X 32
#define FB_BORDER_Y 24

#define AUDIO_RATE 44100
#define AUDIO_MAX_FRAME 2048

/* Keyboard matrix key indices: row*8 + column.  Row 6 lives on port 2. */
enum {
    K_AT = 0, K_A, K_B, K_C, K_D, K_E, K_F, K_G,
    K_H, K_I, K_J, K_K, K_L, K_M, K_N, K_O,
    K_P, K_Q, K_R, K_S, K_T, K_U, K_V, K_W,
    K_X, K_Y, K_Z, K_R3C3, K_R3C4, K_R3C5, K_ENTER, K_SPACE,
    K_0, K_1, K_2, K_3, K_4, K_5, K_6, K_7,
    K_8, K_9, K_COLON, K_SEMI, K_COMMA, K_MINUS, K_PERIOD, K_SLASH,
    K_CONTROL = 48, K_BREAK = 50, K_SHIFT = 55,
    K_COUNT = 56
};

/* palette indices */
enum { C_GREEN, C_YELLOW, C_BLUE, C_RED, C_BUFF, C_CYAN, C_MAGENTA, C_ORANGE,
       C_BLACK, C_DKGREEN, C_DKORANGE, C_NUMCOLORS };

/* RAM expansion in the cartridge slot */
enum { EXP_NONE, EXP_16K, EXP_MCX128, EXP_MCX32SD };

/* bit-banged RS-232 port (Emcee file server link) */
#define SER_MAXEDGES 256
#define SER_RXQ      65536
typedef struct {
    bool     enabled;
    char     target[256];        /* host:port of the TCP peer */
    int      fd;
    unsigned baud;
    /* TxD decoder */
    int      tx_last;
    uint64_t tx_t[SER_MAXEDGES];
    uint8_t  tx_lvl[SER_MAXEDGES];
    int      ntx;
    bool     tx_busy;
    uint64_t tx_start;
    /* RxD encoder */
    uint8_t  rx_q[SER_RXQ];
    int      rx_head, rx_tail;
    bool     rx_busy;
    uint8_t  rx_cur;
    double   rx_start, rx_next;
} serialport;

/* built-in Emcee file server (MCX BASIC serial filing protocol) */
typedef struct {
    bool     used, out;          /* open; opened for output / save */
    int      mode;
    uint8_t *data;               /* input: whole payload */
    size_t   len;
    size_t   boff[512], blen[512];
    uint16_t baddr[512];
    int      nblk, cur, next;
    uint16_t exec;
    void    *fh;                 /* output data file */
    long     lastw;              /* length of the last written block (retry) */
    uint8_t *sbuf;               /* SAVE accumulation */
    size_t   slen, scap;
    char     name[260];
    uint16_t sexec, ssize;
} emfile;

typedef struct {
    bool     on;
    char     root[1024];         /* folder served (host path) */
    char     cwd[1024], progdir[1024];
    int      st;
    uint8_t  cmd, hdr[8];
    int      hneed, hn;
    char     name[260];
    int      nneed, nn;
    uint8_t *data;
    size_t   dneed, dn, dcap;
    emfile   f[16];
    char   (*dir)[40];
    int      ndir, diridx;
} emceesrv;

/* MCX32-SD SD-card controller (ATmega328P) emulation state */
#define SD_MAXFILES 16
typedef struct {
    uint8_t *out;                /* bytes queued for the host */
    size_t   out_len, out_pos, out_cap;
    int      err;                /* error raised once the queue drains */
    int      st;                 /* command parser state */
    uint8_t  prefix, letter, trailer, file;
    char     name[260], name2[260];
    int      nlen, n2len;
    uint8_t  hdr[6];
    int      hn;
    uint32_t arg;
    int      an;
    uint8_t *data;
    size_t   dlen, dneed, dcap;
    char     cwd[1024];          /* current directory, virtual ("/" = card root) */
    char     progdir[1024];      /* directory of the last program LOAD */
    void    *fh[SD_MAXFILES];    /* FILE* of open data files */
    bool     trunc_pending[SD_MAXFILES];
} sdctl;

/* MCX32-SD expansion state */
typedef struct {
    uint8_t  sram[0x10000];     /* indexed by CPU address */
    uint8_t  flash[0x20000];
    char     flash_path[1024];
    bool     fl_dirty, fl_id;
    int      fl_state;
    uint8_t  bank_c, bank_e, mode;
    bool     boot, remap;
} mcx32sd;

typedef struct {
    int      expansion;         /* EXP_NONE, EXP_16K (Tandy) or EXP_MCX128 */
    bool     internal8k;        /* internal 8K RAM mod ($4000-$5FFF, 6K VDG) */
    int      volume;            /* 0..100 */
    bool     sound;
    bool     fast_typing;       /* use ROM keyscan trap for typing/paste */
    bool     lower_as_inverse;  /* quicktype: lowercase -> inverse video */
    char     printer_path[1024];
    char     tape_out_dir[1024];
    char     sd_root[1024];     /* MCX32-SD: host folder acting as the SD card */
    uint32_t palette[C_NUMCOLORS];
} mc10_config;

typedef struct mc10 {
    m6803    cpu;
    mc10_config cfg;

    uint8_t  mem[0x10000];      /* RAM backing (only RAM regions used) */
    uint8_t  rom[0x2000];
    uint8_t  chr[768];          /* internal 6847 character generator */
    uint8_t  chrx[4096];        /* optional external character ROM */
    bool     have_chrx;
    bool     stock_rom;         /* ROM matches MC-10 BASIC 1.0 entry points */

    /* MCX-128 expansion */
    uint8_t  mcx_ram[0x20000];
    uint8_t  mcx_rom[0x4000];
    bool     have_mcx_rom;
    uint8_t  mcx_bank, mcx_romsel;  /* $BF00 / $BF01 */

    mcx32sd  mcx32;
    sdctl    sd;
    serialport serial;
    emceesrv emcee;

    uint8_t  vdg;               /* last value written to $BFFF */
    uint16_t int_ram_end;       /* end of on-board RAM: $5000 or $6000 */
    uint16_t vram_mask;         /* VDG address mask: $0FFF (4K) or $1FFF (8K) */
    uint8_t  keys[8];           /* per column: bit r set = key at row r down */
    uint8_t  joy_keys[8];       /* joystick/typing overlay, ORed into keys */

    /* video */
    uint32_t fb[FB_H][FB_W];
    int      line;
    int      line_cycle;
    uint64_t frame_count;

    /* audio */
    int16_t  audio[AUDIO_MAX_FRAME];
    int      audio_len;
    double   aud_acc, aud_left, aud_lp, aud_hp_in, aud_hp_out;
    int      speaker;

    /* cassette */
    tape_in  tin;
    tape_out tout;
    uint64_t last_port2_read;

    /* printer */
    FILE    *printer;
    int      prn_state, prn_bits, prn_byte;
    int      prn_last_char;

    /* typing queue (quicktype / paste / "sensible" keyboard) */
    uint8_t *typeq;
    size_t   typeq_len, typeq_pos, typeq_cap;
    int      type_phase, type_timer;    /* matrix fallback state */
    uint64_t type_trap_frame;           /* last frame the keyscan hook fired */
    uint64_t kbd_read_frame;            /* last frame software read the key matrix */

    /* debugger hooks */
    uint16_t breakpoints[16];
    int      nbreak;
    bool     break_hit;
    bool     bp_skip;                   /* step over breakpoint at current PC */
    bool     stop_frame;                /* request run_frame to return early */

    char     status[256];               /* last notable event, for the UI */
    bool     status_new;
} mc10;

bool mc10_init(mc10 *m, const mc10_config *cfg);
bool mc10_load_rom(mc10 *m, const char *path);
bool mc10_load_chr(mc10 *m, const char *path);
bool mc10_load_chrx(mc10 *m, const char *path);
bool mc10_load_mcx_rom(mc10 *m, const char *path);
bool mc10_fast_typing_active(mc10 *m);   /* ROM keyscan hook usable now */

/* base MC-10 bus (no cartridge decoding) and the MCX-128 overlay */
uint8_t mc10_base_read(mc10 *m, uint16_t a);
void    mc10_base_write(mc10 *m, uint16_t a, uint8_t v);
uint8_t mcx128_read(mc10 *m, uint16_t a);
void    mcx128_write(mc10 *m, uint16_t a, uint8_t v);
void    mcx128_reset(mc10 *m, bool hard);
uint8_t mcx32sd_read(mc10 *m, uint16_t a);
void    mcx32sd_write(mc10 *m, uint16_t a, uint8_t v);
void    mcx32sd_reset(mc10 *m, bool hard);
bool    mcx32sd_load_flash(mc10 *m, const char *path);
void    mcx32sd_save_flash(mc10 *m);
/* SD controller (sdcard.c) */
uint8_t sd_status(mc10 *m);
uint8_t sd_read(mc10 *m);
void    sd_write(mc10 *m, uint8_t v);
void    sd_control(mc10 *m, uint8_t v);
void    sd_reset(mc10 *m);
void    sd_shutdown(mc10 *m);
/* serial port (serial.c) */
void    serial_init(mc10 *m, const char *target);
void    serial_txd(mc10 *m, int level, uint64_t cycle);
int     serial_rxd(mc10 *m, uint64_t now);
void    serial_poll(mc10 *m);
void    serial_shutdown(mc10 *m);
void    serial_rx_push(mc10 *m, const uint8_t *p, size_t n);
/* built-in Emcee server (sdcard.c) */
void    emcee_byte(mc10 *m, uint8_t b);
void    emcee_shutdown(mc10 *m);
/* exposed for tests / tools */
size_t  sd_tokenize(mc10 *m, const char *text, size_t len, uint8_t *out, size_t cap, uint16_t base);
size_t  sd_detokenize(mc10 *m, const uint8_t *img, size_t len, char *out, size_t cap);
void mc10_reset(mc10 *m, bool hard);
void mc10_run_frame(mc10 *m);            /* emulate one video frame */
void mc10_set_key(mc10 *m, int key, bool down);
void mc10_release_all_keys(mc10 *m);
void mc10_type_text(mc10 *m, const char *text, size_t len);
bool mc10_typing(mc10 *m);
void mc10_status(mc10 *m, const char *fmt, ...);
bool mc10_tape_busy(mc10 *m);            /* tape is being actively read */
void mc10_shutdown(mc10 *m);
bool mc10_char_to_keys(int ch, int *key, bool *shift, bool *ctrl);
void mc10_render_line_for_test(mc10 *m, int line);  /* test hook */

/* binary load / memory save helpers used by debugger & command line */
bool mc10_load_binary(mc10 *m, const char *path, uint16_t addr);
bool mc10_csavem(mc10 *m, const char *path, const char *name,
                 uint16_t start, uint16_t end, uint16_t exec);

#endif
