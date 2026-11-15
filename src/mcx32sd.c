/*
 * mcx32sd.c - Darren Atkinson's MCX32-SD expansion: 32K SRAM, 128K flash
 * (16 banks of 8K) and an SD card handled by an ATmega328P.
 *
 * No hardware documentation is published.  This model was reverse
 * engineered from the MCX BASIC 3.1 BIOS (flash bank 0):
 *
 *   $04  R: SD controller status   W: $80 = abort / resynchronise
 *        bit0 = byte available in $05, bit1 = ready for a byte,
 *        bit2 = controller ready/idle, bit7 = error (code readable in $05)
 *   $05  SD controller data (command bytes, parameters, replies)
 *   $06  flash bank shown at $C000-$DFFF (0 = none)
 *   $07  flash bank shown at $E000-$FFFF (0 = internal BASIC ROM).
 *        From reset until $07 is first written, bank 0 is shown there,
 *        so the CPU fetches its reset vector ($2B00) from the BIOS.
 *   $0F  mode register (BIOS writes $30 on an Alice 32/90, 0 on an MC-10;
 *        bit 4 selects the Alice 1 MHz serial timings)
 *   $1D  flash write enable (written before each flash command write)
 *   $1E  turn the $4200-$43FF -> $0200-$03FF remap off (sticky)
 *   $1F  turn the remap back on
 *        The range is the one the MCX32-SD user guide gives for PEEK@ /
 *        POKE@ (16896-17407): two whole pages covering MicroColor BASIC's
 *        old system variables ($4200-$4345), which MCX BASIC 3 moved down
 *        by $4000.  Reads come from the relocated area in SRAM; writes go
 *        there and through to the MC-10's internal RAM (video).  The
 *        write-through keeps ordinary hi-res programs that draw over this
 *        range on screen, and keeps the program text BASIC stores from
 *        TXTTAB = $4346 identical in both places, so it reads back the same
 *        whether the remap is on or off (MCX BASIC turns it off around
 *        SAVE and LOAD).
 *
 *   $0020-$1FFF  SRAM (MCX BASIC 3 keeps its system variables at $0200)
 *   $2000-$3FFF  flash bank 0: BIOS
 *   $4000-$4FFF  the MC-10's own RAM (video)
 *   $5000-$AFFF  SRAM.  8K + 24K = the board's 32K, and BASIC then reports
 *                27618 bytes free, matching the MCX32-SD user guide.  With
 *                the internal 8K mod, writes to $5000-$5FFF also reach the
 *                internal RAM so the VDG can show PMODE 3/4 screens.
 *   $C000-$FFFF  flash windows (see $06/$07), else the internal ROM
 *
 * Flash is an SST39SF010A (KILL MEM erases an 8K bank as two 4K sectors):
 * unlock $5555/$2AAA, byte program, 4K sector erase and chip erase.
 * Operations complete instantly.
 */
#include "mc10.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SRAM_TOP 0xB000

static bool sd_trace(void)
{
    static int t = -1;
    if (t < 0) t = getenv("VMC10_TRACE_SD") != NULL;
    return t;
}

/* ---------------- flash (29F010 command set) ---------------- */

static void flash_write(mc10 *m, uint32_t fa, uint8_t v)
{
    mcx32sd *x = &m->mcx32;
    uint32_t a = fa & 0x7FFF;              /* unlock addresses decode A0-A14 */
    switch (x->fl_state) {
    case 0:
        if (v == 0xF0) { x->fl_id = false; break; }
        if (a == 0x5555 && v == 0xAA) x->fl_state = 1;
        break;
    case 1:
        x->fl_state = (a == 0x2AAA && v == 0x55) ? 2 : 0;
        break;
    case 2:
        x->fl_state = 0;
        if (a != 0x5555) break;
        if (v == 0xA0) x->fl_state = 3;            /* byte program */
        else if (v == 0x80) x->fl_state = 4;       /* erase setup */
        else if (v == 0x90) x->fl_id = true;       /* autoselect */
        else if (v == 0xF0) x->fl_id = false;
        break;
    case 3:
        x->flash[fa] &= v;                         /* programming only clears bits */
        x->fl_dirty = true;
        x->fl_state = 0;
        break;
    case 4:
        x->fl_state = (a == 0x5555 && v == 0xAA) ? 5 : 0;
        break;
    case 5:
        x->fl_state = (a == 0x2AAA && v == 0x55) ? 6 : 0;
        break;
    case 6:
        x->fl_state = 0;
        if (v == 0x30) {                           /* sector erase (4K) */
            memset(x->flash + (fa & 0x1F000), 0xFF, 0x1000);
            x->fl_dirty = true;
        } else if (v == 0x10 && a == 0x5555) {     /* chip erase */
            memset(x->flash, 0xFF, sizeof x->flash);
            x->fl_dirty = true;
        }
        break;
    }
}

static uint8_t flash_read(mc10 *m, uint32_t fa)
{
    mcx32sd *x = &m->mcx32;
    if (x->fl_id) return (fa & 1) ? 0xB5 : 0xBF;   /* SST39SF010A */
    return x->flash[fa];
}

/* ---------------- bus ---------------- */

static uint16_t remap(mc10 *m, uint16_t a)
{
    if (m->mcx32.remap && a >= 0x4200 && a < 0x4400) return (uint16_t)(a - 0x4000);
    return a;
}

uint8_t mcx32sd_read(mc10 *m, uint16_t a)
{
    mcx32sd *x = &m->mcx32;
    if (a < 0x20) {
        switch (a) {
        case 0x04: return sd_status(m);
        case 0x05: return sd_read(m);
        case 0x06: return x->bank_c;
        case 0x07: return x->bank_e;
        case 0x0F: return x->mode;
        default:   return 0xFF;
        }
    }
    if (a >= 0x80 && a < 0x100) return m->mem[a];          /* on-chip RAM */
    if (a < 0x2000) return x->sram[a];
    if (a < 0x4000) return flash_read(m, a - 0x2000);      /* BIOS, bank 0 */
    if (a < SRAM_TOP) {
        uint16_t r = remap(m, a);
        if (r >= 0x4000 && r < 0x5000) return mc10_base_read(m, r);   /* internal RAM */
        return x->sram[r];
    }
    if (a < 0xC000) return mc10_base_read(m, a);           /* keyboard etc. */
    if (a < 0xE000) {
        if (x->bank_c) return flash_read(m, (uint32_t)(x->bank_c & 15) * 0x2000 + (a - 0xC000));
        return mc10_base_read(m, a);
    }
    if (x->boot) return flash_read(m, a - 0xE000);
    if (x->bank_e) return flash_read(m, (uint32_t)(x->bank_e & 15) * 0x2000 + (a - 0xE000));
    return mc10_base_read(m, a);
}

void mcx32sd_write(mc10 *m, uint16_t a, uint8_t v)
{
    mcx32sd *x = &m->mcx32;
    if (a < 0x20) {
        switch (a) {
        case 0x04: sd_control(m, v); break;
        case 0x05: sd_write(m, v); break;
        case 0x06: x->bank_c = v; break;
        case 0x07: x->bank_e = v; x->boot = false; break;
        case 0x0F: x->mode = v; break;
        case 0x1D: break;                          /* flash write enable */
        case 0x1E: x->remap = false; if (sd_trace()) fprintf(stderr, "remap OFF at %04X\n", m->cpu.ppc); break;
        case 0x1F: x->remap = true; if (sd_trace()) fprintf(stderr, "remap ON  at %04X\n", m->cpu.ppc); break;
        }
        return;
    }
    if (a >= 0x80 && a < 0x100) { m->mem[a] = v; x->sram[a] = v; return; }
    if (a < 0x2000) { x->sram[a] = v; return; }
    if (a < 0x4000) { flash_write(m, a - 0x2000, v); return; }
    if (a < SRAM_TOP) {
        uint16_t r = remap(m, a);
        if (r != a) {
            /* Remapped system-variable address: the write goes to the
             * relocated variables in SRAM *and* through to the MC-10's own
             * RAM, so programs that draw graphics over $4200-$43FF (any
             * hi-res game written for a plain MC-10) still see their
             * pixels on screen, while reads keep returning the variables
             * that the user guide says are mapped there. */
            x->sram[r] = v;
            mc10_base_write(m, a, v);
            return;
        }
        if (r >= 0x4000 && r < 0x6000) mc10_base_write(m, r, v);   /* video RAM */
        if (r >= 0x5000 || r < 0x4000) x->sram[r] = v;
        return;
    }
    if (a < 0xC000) { mc10_base_write(m, a, v); return; }
    if (a < 0xE000) {
        if (x->bank_c) flash_write(m, (uint32_t)(x->bank_c & 15) * 0x2000 + (a - 0xC000), v);
        return;
    }
    if (x->bank_e && !x->boot) flash_write(m, (uint32_t)(x->bank_e & 15) * 0x2000 + (a - 0xE000), v);
}

void mcx32sd_reset(mc10 *m, bool hard)
{
    mcx32sd *x = &m->mcx32;
    x->bank_c = x->bank_e = 0;
    x->mode = 0;
    x->boot = true;
    x->remap = false;
    x->fl_state = 0;
    x->fl_id = false;
    if (hard) memset(x->sram, 0, sizeof x->sram);
    sd_reset(m);
}

bool mcx32sd_load_flash(mc10 *m, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(m->mcx32.flash, 1, sizeof m->mcx32.flash, f);
    fclose(f);
    if (n != sizeof m->mcx32.flash) return false;
    snprintf(m->mcx32.flash_path, sizeof m->mcx32.flash_path, "%s", path);
    m->mcx32.fl_dirty = false;
    return true;
}

void mcx32sd_save_flash(mc10 *m)
{
    mcx32sd *x = &m->mcx32;
    if (!x->fl_dirty || !x->flash_path[0]) return;
    FILE *f = fopen(x->flash_path, "wb");
    if (f) {
        fwrite(x->flash, 1, sizeof x->flash, f);
        fclose(f);
        x->fl_dirty = false;
        if (sd_trace()) fprintf(stderr, "mcx32: flash saved to %s\n", x->flash_path);
    }
}
