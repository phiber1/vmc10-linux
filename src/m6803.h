/*
 * m6803.h - Motorola MC6801/6803 CPU core (instruction set + on-chip timer
 * and I/O port registers) for the VMC10 Linux port.
 *
 * Written from scratch from the Motorola MC6801/6803 data sheet; cycle
 * counts follow the 6803 timing table.
 */
#ifndef M6803_H
#define M6803_H

#include <stdint.h>
#include <stdbool.h>

/* Condition code bits */
#define CC_C 0x01
#define CC_V 0x02
#define CC_Z 0x04
#define CC_N 0x08
#define CC_I 0x10
#define CC_H 0x20

/* Timer control/status register bits ($08) */
#define TCSR_OLVL 0x01
#define TCSR_IEDG 0x02
#define TCSR_ETOI 0x04
#define TCSR_EOCI 0x08
#define TCSR_EICI 0x10
#define TCSR_TOF  0x20
#define TCSR_OCF  0x40
#define TCSR_ICF  0x80

typedef struct m6803 m6803;

struct m6803 {
    uint8_t  a, b, cc;
    uint16_t x, sp, pc;
    uint16_t ppc;            /* address of the instruction being executed */
    uint64_t cycles;         /* total E-clock cycles executed */
    bool     wai;            /* waiting for interrupt (WAI) */
    bool     irq_line;       /* external IRQ1 level (active = true) */
    bool     nmi_pending;

    /* on-chip registers */
    uint8_t  ddr1, ddr2, port1_out, port2_out;
    uint16_t counter, ocr;
    uint8_t  tcsr;
    uint8_t  tcsr_read_flags; /* flags that were set when TCSR was last read */
    uint8_t  regs[0x20];      /* backing store for the rarely-used rest */

    /* bus callbacks (addresses outside $0000-$001F) */
    void    *ctx;
    uint8_t (*read)(void *ctx, uint16_t addr);
    void    (*write)(void *ctx, uint16_t addr, uint8_t val);
    /* port callbacks: return the value present on the external pins */
    uint8_t (*port_read)(void *ctx, int port);
    void    (*port_write)(void *ctx, int port, uint8_t val, uint8_t ddr);

    uint32_t illegal_count;
    uint8_t  last_illegal;
};

void     m6803_init(m6803 *c);
void     m6803_reset(m6803 *c);
int      m6803_step(m6803 *c);   /* execute one instruction; returns cycles */
uint8_t  m6803_rd(m6803 *c, uint16_t addr);  /* full bus read incl. regs */
void     m6803_wr(m6803 *c, uint16_t addr, uint8_t v);
uint8_t  m6803_peek(m6803 *c, uint16_t addr); /* side-effect-free read */

/* disassembler: writes text into buf, returns instruction length */
int      m6803_disasm(m6803 *c, uint16_t pc, char *buf, int buflen);

#endif
