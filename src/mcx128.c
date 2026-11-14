/*
 * mcx128.c - Darren Atkinson's MCX-128 expansion: 128K banked RAM + 16K EPROM
 * (MCX BASIC).
 *
 * This follows the board's GAL22v10 equations (MCX128.pld, ATK Software,
 * 2010; see linux/docs/MCX128_Essentials):
 *
 *   Pages:  0000-3FFF RAM, A16 = P0
 *           4000-7FFF RAM, A16 = P1, except the MC-10's own RAM at
 *                     4000-4FFF (4000-5FFF with the 8K GAL) while P1 = 0
 *           8000-BEFF RAM, A16 = P1
 *           BF00-BF7F registers: even address = RAM bank (P0,P1),
 *                                odd address  = ROM map (M0,M1)
 *           BF80-BFFF MC-10 keyboard / VDG / sound
 *           C000-FFFF ROM map mode (writes always go to RAM, A16 = P0,
 *                     except FF00-FFFF which always uses A16 = 0):
 *              0: 16K external EPROM
 *              1: RAM at C000-DFFF, EPROM at E000-FFFF
 *              2: RAM at C000-DFFF, internal BASIC ROM at E000-FFFF
 *              3: RAM throughout
 *
 * The SRAM is addressed by the CPU address plus A16, so the expansion RAM
 * array is indexed as (A16 << 16) | address.  Both registers clear on reset.
 */
#include "mc10.h"
#include <string.h>


#define P0(m) ((m)->mcx_bank & 1)
#define P1(m) (((m)->mcx_bank >> 1) & 1)
#define M(m)  ((m)->mcx_romsel & 3)

static inline uint8_t *ram(mc10 *m, unsigned a16, uint16_t a)
{
    return &m->mcx_ram[((uint32_t)a16 << 16) | a];
}

static inline bool internal_ram(mc10 *m, uint16_t a)
{
    uint16_t end = m->cfg.internal8k ? 0x6000 : 0x5000;
    return !P1(m) && a >= 0x4000 && a < end;
}

uint8_t mcx128_read(mc10 *m, uint16_t a)
{
    if (a >= 0x80 && a < 0x100) return m->mem[a];         /* 6803 on-chip RAM */
    if (a < 0x4000) return *ram(m, P0(m), a);
    if (a < 0xBF00) {
        if (internal_ram(m, a)) return mc10_base_read(m, a);
        return *ram(m, P1(m), a);
    }
    if (a < 0xBF80) return (a & 1) ? M(m) : (m->mcx_bank & 3);
    if (a < 0xC000) return mc10_base_read(m, a);          /* keyboard */

    switch (M(m)) {
    case 0:  return m->mcx_rom[a & 0x3FFF];
    case 1:  if (a < 0xE000) break;
             return m->mcx_rom[a & 0x3FFF];
    case 2:  if (a < 0xE000) break;
             return m->rom[a & 0x1FFF];
    default: break;
    }
    return *ram(m, a >= 0xFF00 ? 0 : P0(m), a);
}

void mcx128_write(mc10 *m, uint16_t a, uint8_t v)
{
    if (a < 0x4000) {
        *ram(m, P0(m), a) = v;                            /* incl. $80-$FF mirror */
        if (a >= 0x80 && a < 0x100) m->mem[a] = v;
        return;
    }
    if (a < 0xBF00) {
        if (internal_ram(m, a)) mc10_base_write(m, a, v);
        else *ram(m, P1(m), a) = v;
        return;
    }
    if (a < 0xBF80) {
        if (a & 1) m->mcx_romsel = v & 3;
        else       m->mcx_bank = v & 3;
        return;
    }
    if (a < 0xC000) { mc10_base_write(m, a, v); return; }  /* VDG / sound latch */
    *ram(m, a >= 0xFF00 ? 0 : P0(m), a) = v;
}

void mcx128_reset(mc10 *m, bool hard)
{
    m->mcx_bank = 0;
    m->mcx_romsel = 0;
    if (hard) memset(m->mcx_ram, 0, sizeof m->mcx_ram);
}
