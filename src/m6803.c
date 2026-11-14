/*
 * m6803.c - Motorola MC6801/6803 CPU core.
 *
 * Implements the full documented 6801/6803 instruction set, the
 * on-chip I/O port registers, and the 16-bit free-running timer with
 * output compare and overflow interrupts.  The serial unit (SCI) is
 * registered but inert, as on the original VMC10.
 */
#include "m6803.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- 6803 cycle table (0 = illegal opcode) ---- */
static const uint8_t cyc[256] = {
/*        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
/*0*/     2, 2, 0, 0, 3, 3, 2, 2, 3, 3, 2, 2, 2, 2, 2, 2,
/*1*/     2, 2, 0, 0, 0, 0, 2, 2, 0, 2, 0, 2, 0, 0, 0, 0,
/*2*/     3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
/*3*/     3, 3, 4, 4, 3, 3, 3, 3, 5, 5, 3,10, 4,10, 9,12,
/*4*/     2, 0, 0, 2, 2, 0, 2, 2, 2, 2, 2, 0, 2, 2, 0, 2,
/*5*/     2, 0, 0, 2, 2, 0, 2, 2, 2, 2, 2, 0, 2, 2, 0, 2,
/*6*/     6, 0, 0, 6, 6, 0, 6, 6, 6, 6, 6, 0, 6, 6, 3, 6,
/*7*/     6, 0, 0, 6, 6, 0, 6, 6, 6, 6, 6, 0, 6, 6, 3, 6,
/*8*/     2, 2, 2, 4, 2, 2, 2, 0, 2, 2, 2, 2, 4, 6, 3, 0,
/*9*/     3, 3, 3, 5, 3, 3, 3, 3, 3, 3, 3, 3, 5, 5, 4, 4,
/*A*/     4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 6, 6, 5, 5,
/*B*/     4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 6, 6, 5, 5,
/*C*/     2, 2, 2, 4, 2, 2, 2, 0, 2, 2, 2, 2, 3, 0, 3, 0,
/*D*/     3, 3, 3, 5, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4,
/*E*/     4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5,
/*F*/     4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5,
};

/* ---------------- bus ---------------- */

static uint8_t port_value(m6803 *c, int port)
{
    uint8_t out = port ? c->port2_out : c->port1_out;
    uint8_t ddr = port ? c->ddr2 : c->ddr1;
    uint8_t pins = c->port_read ? c->port_read(c->ctx, port) : 0xFF;
    uint8_t v = (out & ddr) | (pins & ~ddr);
    if (port) v = (v & 0x1F) | (pins & 0xE0);   /* P2 has only 5 lines */
    return v;
}

static uint8_t reg_read(m6803 *c, uint8_t r, bool side_effects)
{
    switch (r) {
    case 0x00: return c->ddr1;
    case 0x01: return c->ddr2;
    case 0x02: return port_value(c, 0);
    case 0x03: return port_value(c, 1);
    case 0x08:
        if (side_effects) c->tcsr_read_flags = c->tcsr & (TCSR_TOF | TCSR_OCF | TCSR_ICF);
        return c->tcsr;
    case 0x09:
        if (side_effects && (c->tcsr_read_flags & TCSR_TOF)) {
            c->tcsr &= ~TCSR_TOF;
            c->tcsr_read_flags &= ~TCSR_TOF;
        }
        return c->counter >> 8;
    case 0x0A: return c->counter & 0xFF;
    case 0x0B: return c->ocr >> 8;
    case 0x0C: return c->ocr & 0xFF;
    case 0x11: return c->regs[0x11] | 0x20;   /* SCI: transmit reg empty */
    default:   return c->regs[r];
    }
}

static void reg_write(m6803 *c, uint8_t r, uint8_t v)
{
    c->regs[r] = v;
    switch (r) {
    case 0x00: c->ddr1 = v;
        if (c->port_write) c->port_write(c->ctx, 0, c->port1_out, c->ddr1);
        break;
    case 0x01: c->ddr2 = v;
        if (c->port_write) c->port_write(c->ctx, 1, c->port2_out, c->ddr2);
        break;
    case 0x02: c->port1_out = v;
        if (c->port_write) c->port_write(c->ctx, 0, v, c->ddr1);
        break;
    case 0x03: c->port2_out = v;
        if (c->port_write) c->port_write(c->ctx, 1, v, c->ddr2);
        break;
    case 0x08: c->tcsr = (c->tcsr & 0xE0) | (v & 0x1F); break;
    case 0x09: c->counter = 0xFFF8; break;     /* 6801 quirk */
    case 0x0B: case 0x0C:
        if (r == 0x0B) c->ocr = (c->ocr & 0x00FF) | (v << 8);
        else           c->ocr = (c->ocr & 0xFF00) | v;
        if (c->tcsr_read_flags & TCSR_OCF) {
            c->tcsr &= ~TCSR_OCF;
            c->tcsr_read_flags &= ~TCSR_OCF;
        }
        break;
    default: break;
    }
}

/* On the 6803 the 6801's port 3/4 registers ($04-$07), the port 3 control
 * register ($0F) and the reserved block ($15-$1F) are not decoded on chip,
 * so those addresses go out to the external bus (expansion hardware such
 * as the MCX boards puts registers there). */
static inline bool internal_reg(uint16_t a)
{
    return a < 0x20 && !(a >= 0x04 && a <= 0x07) && a != 0x0F && a < 0x15;
}

uint8_t m6803_rd(m6803 *c, uint16_t a)
{
    if (internal_reg(a)) return reg_read(c, (uint8_t)a, true);
    return c->read(c->ctx, a);
}

void m6803_wr(m6803 *c, uint16_t a, uint8_t v)
{
    if (internal_reg(a)) { reg_write(c, (uint8_t)a, v); return; }
    c->write(c->ctx, a, v);
}

uint8_t m6803_peek(m6803 *c, uint16_t a)
{
    if (internal_reg(a)) return reg_read(c, (uint8_t)a, false);
    return c->read(c->ctx, a);
}

#define RD(a)     m6803_rd(c, (uint16_t)(a))
#define WR(a, v)  m6803_wr(c, (uint16_t)(a), (uint8_t)(v))

static inline uint16_t rd16(m6803 *c, uint16_t a)
{
    uint16_t hi = RD(a);
    return (uint16_t)((hi << 8) | RD(a + 1));
}
static inline void wr16(m6803 *c, uint16_t a, uint16_t v)
{
    WR(a, v >> 8);
    WR(a + 1, v & 0xFF);
}
static inline uint8_t fetch(m6803 *c) { return RD(c->pc++); }
static inline uint16_t fetch16(m6803 *c) { uint16_t v = rd16(c, c->pc); c->pc += 2; return v; }

static inline void push8(m6803 *c, uint8_t v) { WR(c->sp, v); c->sp--; }
static inline uint8_t pull8(m6803 *c) { c->sp++; return RD(c->sp); }
static inline void push16(m6803 *c, uint16_t v) { push8(c, v & 0xFF); push8(c, v >> 8); }
static inline uint16_t pull16(m6803 *c) { uint16_t hi = pull8(c); return (uint16_t)((hi << 8) | pull8(c)); }

/* ---------------- flags ---------------- */

#define SETF(f, cond) do { if (cond) c->cc |= (f); else c->cc &= ~(f); } while (0)

static inline void nz8(m6803 *c, uint8_t r)
{
    c->cc &= ~(CC_N | CC_Z);
    if (r & 0x80) c->cc |= CC_N;
    if (!r) c->cc |= CC_Z;
}
static inline void nz16(m6803 *c, uint16_t r)
{
    c->cc &= ~(CC_N | CC_Z);
    if (r & 0x8000) c->cc |= CC_N;
    if (!r) c->cc |= CC_Z;
}
static inline void ld8(m6803 *c, uint8_t r) { nz8(c, r); c->cc &= ~CC_V; }
static inline void ld16(m6803 *c, uint16_t r) { nz16(c, r); c->cc &= ~CC_V; }

static uint8_t op_add(m6803 *c, uint8_t a, uint8_t b, int carry)
{
    unsigned r = a + b + carry;
    c->cc &= ~(CC_H | CC_N | CC_Z | CC_V | CC_C);
    if ((a ^ b ^ r) & 0x10) c->cc |= CC_H;
    if (r & 0x80) c->cc |= CC_N;
    if (!(r & 0xFF)) c->cc |= CC_Z;
    if ((a ^ r) & (b ^ r) & 0x80) c->cc |= CC_V;
    if (r & 0x100) c->cc |= CC_C;
    return (uint8_t)r;
}
static uint8_t op_sub(m6803 *c, uint8_t a, uint8_t b, int carry)
{
    unsigned r = (unsigned)a - b - carry;
    c->cc &= ~(CC_N | CC_Z | CC_V | CC_C);
    if (r & 0x80) c->cc |= CC_N;
    if (!(r & 0xFF)) c->cc |= CC_Z;
    if ((a ^ b) & (a ^ r) & 0x80) c->cc |= CC_V;
    if (r & 0x100) c->cc |= CC_C;
    return (uint8_t)r;
}
static uint16_t op_sub16(m6803 *c, uint16_t a, uint16_t b)
{
    uint32_t r = (uint32_t)a - b;
    c->cc &= ~(CC_N | CC_Z | CC_V | CC_C);
    if (r & 0x8000) c->cc |= CC_N;
    if (!(r & 0xFFFF)) c->cc |= CC_Z;
    if ((a ^ b) & (a ^ r) & 0x8000) c->cc |= CC_V;
    if (r & 0x10000) c->cc |= CC_C;
    return (uint16_t)r;
}
static uint16_t op_add16(m6803 *c, uint16_t a, uint16_t b)
{
    uint32_t r = (uint32_t)a + b;
    c->cc &= ~(CC_N | CC_Z | CC_V | CC_C);
    if (r & 0x8000) c->cc |= CC_N;
    if (!(r & 0xFFFF)) c->cc |= CC_Z;
    if ((a ^ r) & (b ^ r) & 0x8000) c->cc |= CC_V;
    if (r & 0x10000) c->cc |= CC_C;
    return (uint16_t)r;
}

/* read-modify-write unary operations; op = low nibble of opcode */
static uint8_t op_unary(m6803 *c, int op, uint8_t m)
{
    uint8_t r = m;
    int cin = c->cc & CC_C;
    switch (op) {
    case 0x0: /* NEG */
        r = (uint8_t)(0 - m);
        nz8(c, r); SETF(CC_V, r == 0x80); SETF(CC_C, r != 0);
        break;
    case 0x3: /* COM */
        r = (uint8_t)~m;
        nz8(c, r); c->cc &= ~CC_V; c->cc |= CC_C;
        break;
    case 0x4: /* LSR */
        r = m >> 1;
        nz8(c, r); SETF(CC_C, m & 1); SETF(CC_V, m & 1);
        break;
    case 0x6: /* ROR */
        r = (uint8_t)((m >> 1) | (cin ? 0x80 : 0));
        nz8(c, r); SETF(CC_C, m & 1);
        SETF(CC_V, ((r >> 7) ^ (m & 1)) & 1);
        break;
    case 0x7: /* ASR */
        r = (uint8_t)((m >> 1) | (m & 0x80));
        nz8(c, r); SETF(CC_C, m & 1);
        SETF(CC_V, ((r >> 7) ^ (m & 1)) & 1);
        break;
    case 0x8: /* ASL */
        r = (uint8_t)(m << 1);
        nz8(c, r); SETF(CC_C, m & 0x80);
        SETF(CC_V, ((r >> 7) ^ (m >> 7)) & 1);
        break;
    case 0x9: /* ROL */
        r = (uint8_t)((m << 1) | (cin ? 1 : 0));
        nz8(c, r); SETF(CC_C, m & 0x80);
        SETF(CC_V, ((r >> 7) ^ (m >> 7)) & 1);
        break;
    case 0xA: /* DEC */
        r = m - 1;
        nz8(c, r); SETF(CC_V, m == 0x80);
        break;
    case 0xC: /* INC */
        r = m + 1;
        nz8(c, r); SETF(CC_V, m == 0x7F);
        break;
    case 0xD: /* TST */
        nz8(c, m); c->cc &= ~(CC_V | CC_C);
        break;
    case 0xF: /* CLR */
        r = 0;
        c->cc &= ~(CC_N | CC_V | CC_C); c->cc |= CC_Z;
        break;
    }
    return r;
}

/* accumulator ALU ops; op = low nibble of opcode 0x80-0xFF group */
static void op_alu(m6803 *c, int op, uint8_t *acc, uint8_t m)
{
    switch (op) {
    case 0x0: *acc = op_sub(c, *acc, m, 0); break;                 /* SUB */
    case 0x1: op_sub(c, *acc, m, 0); break;                        /* CMP */
    case 0x2: *acc = op_sub(c, *acc, m, c->cc & CC_C); break;      /* SBC */
    case 0x4: *acc &= m; ld8(c, *acc); break;                      /* AND */
    case 0x5: ld8(c, *acc & m); break;                             /* BIT */
    case 0x6: *acc = m; ld8(c, m); break;                          /* LDA */
    case 0x8: *acc ^= m; ld8(c, *acc); break;                      /* EOR */
    case 0x9: *acc = op_add(c, *acc, m, c->cc & CC_C); break;      /* ADC */
    case 0xA: *acc |= m; ld8(c, *acc); break;                      /* ORA */
    case 0xB: *acc = op_add(c, *acc, m, 0); break;                 /* ADD */
    }
}

/* ---------------- interrupts / timer ---------------- */

static void take_interrupt(m6803 *c, uint16_t vector)
{
    if (!c->wai) {
        push16(c, c->pc);
        push16(c, c->x);
        push8(c, c->a);
        push8(c, c->b);
        push8(c, c->cc);
        c->cycles += 12;
    }
    c->wai = false;
    c->cc |= CC_I;
    c->pc = rd16(c, vector);
}

static void timer_advance(m6803 *c, int n)
{
    uint16_t old = c->counter;
    uint16_t dist = (uint16_t)(c->ocr - old);   /* cycles until match */
    if (dist != 0 && dist <= n) c->tcsr |= TCSR_OCF;
    if ((uint32_t)old + n > 0xFFFF) c->tcsr |= TCSR_TOF;
    c->counter = (uint16_t)(old + n);
}

static int check_interrupts(m6803 *c)
{
    if (c->nmi_pending) {
        c->nmi_pending = false;
        take_interrupt(c, 0xFFFC);
        return 1;
    }
    if (c->cc & CC_I) return 0;
    if (c->irq_line)                                         { take_interrupt(c, 0xFFF8); return 1; }
    if ((c->tcsr & TCSR_ICF) && (c->tcsr & TCSR_EICI))       { take_interrupt(c, 0xFFF6); return 1; }
    if ((c->tcsr & TCSR_OCF) && (c->tcsr & TCSR_EOCI))       { take_interrupt(c, 0xFFF4); return 1; }
    if ((c->tcsr & TCSR_TOF) && (c->tcsr & TCSR_ETOI))       { take_interrupt(c, 0xFFF2); return 1; }
    return 0;
}

/* ---------------- public ---------------- */

void m6803_init(m6803 *c)
{
    void *ctx = c->ctx;
    uint8_t (*r)(void *, uint16_t) = c->read;
    void (*w)(void *, uint16_t, uint8_t) = c->write;
    uint8_t (*pr)(void *, int) = c->port_read;
    void (*pw)(void *, int, uint8_t, uint8_t) = c->port_write;
    memset(c, 0, sizeof *c);
    c->ctx = ctx; c->read = r; c->write = w;
    c->port_read = pr; c->port_write = pw;
}

void m6803_reset(m6803 *c)
{
    c->ddr1 = c->ddr2 = 0;
    c->port1_out = c->port2_out = 0;
    c->counter = 0;
    c->ocr = 0xFFFF;
    c->tcsr = 0;
    c->tcsr_read_flags = 0;
    c->cc = 0xC0 | CC_I;
    c->wai = false;
    c->nmi_pending = false;
    c->pc = rd16(c, 0xFFFE);
}

int m6803_step(m6803 *c)
{
    uint64_t start = c->cycles;

    if (check_interrupts(c)) {
        int n = (int)(c->cycles - start);
        timer_advance(c, n);
        return n;
    }
    if (c->wai) {                 /* idle until an interrupt arrives */
        c->cycles += 1;
        timer_advance(c, 1);
        return 1;
    }

    c->ppc = c->pc;
    uint8_t op = fetch(c);
    int n = cyc[op];
    uint16_t ea = 0;
    uint8_t t8;
    uint16_t t16;

    switch (op) {
    /* ---- inherent ---- */
    case 0x01: break;                                              /* NOP */
    case 0x04: t16 = (uint16_t)((c->a << 8) | c->b);               /* LSRD */
        SETF(CC_C, t16 & 1); t16 >>= 1; nz16(c, t16); SETF(CC_V, c->cc & CC_C);
        c->a = t16 >> 8; c->b = t16 & 0xFF; break;
    case 0x05: t16 = (uint16_t)((c->a << 8) | c->b);               /* ASLD */
        SETF(CC_C, t16 & 0x8000); t16 <<= 1; nz16(c, t16);
        SETF(CC_V, ((t16 >> 15) ^ ((c->cc & CC_C) ? 1 : 0)) & 1);
        c->a = t16 >> 8; c->b = t16 & 0xFF; break;
    case 0x06: c->cc = c->a | 0xC0; break;                         /* TAP */
    case 0x07: c->a = c->cc | 0xC0; break;                         /* TPA */
    case 0x08: c->x++; SETF(CC_Z, c->x == 0); break;               /* INX */
    case 0x09: c->x--; SETF(CC_Z, c->x == 0); break;               /* DEX */
    case 0x0A: c->cc &= ~CC_V; break;
    case 0x0B: c->cc |= CC_V; break;
    case 0x0C: c->cc &= ~CC_C; break;
    case 0x0D: c->cc |= CC_C; break;
    case 0x0E: c->cc &= ~CC_I; break;
    case 0x0F: c->cc |= CC_I; break;
    case 0x10: c->a = op_sub(c, c->a, c->b, 0); break;             /* SBA */
    case 0x11: op_sub(c, c->a, c->b, 0); break;                    /* CBA */
    case 0x16: c->b = c->a; ld8(c, c->b); break;                   /* TAB */
    case 0x17: c->a = c->b; ld8(c, c->a); break;                   /* TBA */
    case 0x19: {                                                   /* DAA */
        unsigned a = c->a, adj = 0;
        int cf = c->cc & CC_C;
        if ((c->cc & CC_H) || (a & 0x0F) > 9) adj |= 0x06;
        if (cf || a > 0x99 || ((a & 0xF0) > 0x80 && (a & 0x0F) > 9)) { adj |= 0x60; cf = 1; }
        a += adj;
        c->a = (uint8_t)a;
        nz8(c, c->a); SETF(CC_C, cf || (a & 0x100));
        c->cc &= ~CC_V;
        break; }
    case 0x1B: c->a = op_add(c, c->a, c->b, 0); break;             /* ABA */

    /* ---- branches ---- */
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F: {
        int8_t off = (int8_t)fetch(c);
        int ccv = c->cc, take = 0;
        int N = !!(ccv & CC_N), Z = !!(ccv & CC_Z), V = !!(ccv & CC_V), C = !!(ccv & CC_C);
        switch (op & 0x0F) {
        case 0x0: take = 1; break;
        case 0x1: take = 0; break;
        case 0x2: take = !(C | Z); break;
        case 0x3: take = C | Z; break;
        case 0x4: take = !C; break;
        case 0x5: take = C; break;
        case 0x6: take = !Z; break;
        case 0x7: take = Z; break;
        case 0x8: take = !V; break;
        case 0x9: take = V; break;
        case 0xA: take = !N; break;
        case 0xB: take = N; break;
        case 0xC: take = !(N ^ V); break;
        case 0xD: take = N ^ V; break;
        case 0xE: take = !(Z | (N ^ V)); break;
        case 0xF: take = Z | (N ^ V); break;
        }
        if (take) c->pc = (uint16_t)(c->pc + off);
        break; }

    /* ---- stack / misc ---- */
    case 0x30: c->x = c->sp + 1; break;                            /* TSX */
    case 0x31: c->sp++; break;                                     /* INS */
    case 0x32: c->a = pull8(c); break;                             /* PULA */
    case 0x33: c->b = pull8(c); break;                             /* PULB */
    case 0x34: c->sp--; break;                                     /* DES */
    case 0x35: c->sp = c->x - 1; break;                            /* TXS */
    case 0x36: push8(c, c->a); break;                              /* PSHA */
    case 0x37: push8(c, c->b); break;                              /* PSHB */
    case 0x38: c->x = pull16(c); break;                            /* PULX */
    case 0x39: c->pc = pull16(c); break;                           /* RTS */
    case 0x3A: c->x += c->b; break;                                /* ABX */
    case 0x3B:                                                     /* RTI */
        c->cc = pull8(c) | 0xC0;
        c->b = pull8(c);
        c->a = pull8(c);
        c->x = pull16(c);
        c->pc = pull16(c);
        break;
    case 0x3C: push16(c, c->x); break;                             /* PSHX */
    case 0x3D: t16 = (uint16_t)(c->a * c->b);                      /* MUL */
        c->a = t16 >> 8; c->b = t16 & 0xFF; SETF(CC_C, c->b & 0x80); break;
    case 0x3E:                                                     /* WAI */
        push16(c, c->pc); push16(c, c->x);
        push8(c, c->a); push8(c, c->b); push8(c, c->cc);
        c->wai = true;
        break;
    case 0x3F:                                                     /* SWI */
        push16(c, c->pc); push16(c, c->x);
        push8(c, c->a); push8(c, c->b); push8(c, c->cc);
        c->cc |= CC_I;
        c->pc = rd16(c, 0xFFFA);
        break;

    /* ---- accumulator unary ---- */
    case 0x40: case 0x43: case 0x44: case 0x46: case 0x47: case 0x48:
    case 0x49: case 0x4A: case 0x4C: case 0x4D: case 0x4F:
        c->a = op_unary(c, op & 0x0F, c->a); break;
    case 0x50: case 0x53: case 0x54: case 0x56: case 0x57: case 0x58:
    case 0x59: case 0x5A: case 0x5C: case 0x5D: case 0x5F:
        c->b = op_unary(c, op & 0x0F, c->b); break;

    /* ---- memory unary (indexed / extended) ---- */
    case 0x60: case 0x63: case 0x64: case 0x66: case 0x67: case 0x68:
    case 0x69: case 0x6A: case 0x6C: case 0x6D: case 0x6F:
    case 0x70: case 0x73: case 0x74: case 0x76: case 0x77: case 0x78:
    case 0x79: case 0x7A: case 0x7C: case 0x7D: case 0x7F:
        ea = (op & 0x10) ? fetch16(c) : (uint16_t)(c->x + fetch(c));
        if ((op & 0x0F) == 0x0F) { op_unary(c, 0xF, 0); WR(ea, 0); }
        else {
            t8 = RD(ea);
            t8 = op_unary(c, op & 0x0F, t8);
            if ((op & 0x0F) != 0x0D) WR(ea, t8);
        }
        break;
    case 0x6E: c->pc = (uint16_t)(c->x + fetch(c)); break;         /* JMP ix */
    case 0x7E: c->pc = fetch16(c); break;                          /* JMP ext */

    /* ---- special 16-bit and control in the 8x-Fx block ---- */
    case 0x8D: { int8_t off = (int8_t)fetch(c);                    /* BSR */
        push16(c, c->pc); c->pc = (uint16_t)(c->pc + off); break; }
    case 0x9D: case 0xAD: case 0xBD:                               /* JSR */
        if (op == 0x9D) ea = fetch(c);
        else if (op == 0xAD) ea = (uint16_t)(c->x + fetch(c));
        else ea = fetch16(c);
        push16(c, c->pc); c->pc = ea; break;

    default:
        if (op >= 0x80) {
            int mode = (op >> 4) & 3;          /* 0 imm, 1 dir, 2 idx, 3 ext */
            int lo = op & 0x0F;
            bool is_b = op >= 0xC0;
            bool wide = false;                 /* 16-bit operand? */

            /* identify 16-bit ops */
            if (lo == 0x3 || lo == 0xC || lo == 0xE) wide = true;  /* SUBD/ADDD, CPX/LDD, LDS/LDX */
            if (lo == 0xD && is_b) wide = true;                    /* STD */
            if (lo == 0xF) wide = true;                            /* STS/STX */

            /* immediate store opcodes are illegal */
            if (mode == 0 && (lo == 0x7 || lo == 0xF || (lo == 0xD && is_b))) goto illegal;

            if (mode == 0) { ea = c->pc; c->pc += wide ? 2 : 1; }
            else if (mode == 1) ea = fetch(c);
            else if (mode == 2) ea = (uint16_t)(c->x + fetch(c));
            else ea = fetch16(c);

            uint16_t d = (uint16_t)((c->a << 8) | c->b);
            switch (lo) {
            case 0x3:
                t16 = rd16(c, ea);
                d = is_b ? op_add16(c, d, t16) : op_sub16(c, d, t16);
                c->a = d >> 8; c->b = d & 0xFF;
                break;
            case 0x7:
                t8 = is_b ? c->b : c->a;
                WR(ea, t8); ld8(c, t8);
                break;
            case 0xC:
                t16 = rd16(c, ea);
                if (is_b) { c->a = t16 >> 8; c->b = t16 & 0xFF; ld16(c, t16); }  /* LDD */
                else op_sub16(c, c->x, t16);                                  /* CPX */
                break;
            case 0xD:                                                         /* STD */
                wr16(c, ea, d); ld16(c, d);
                break;
            case 0xE:
                t16 = rd16(c, ea);
                if (is_b) c->x = t16; else c->sp = t16;
                ld16(c, t16);
                break;
            case 0xF:
                t16 = is_b ? c->x : c->sp;
                wr16(c, ea, t16); ld16(c, t16);
                break;
            default:
                op_alu(c, lo, is_b ? &c->b : &c->a, RD(ea));
                break;
            }
            break;
        }
    illegal:
        /* Undocumented opcode: treat as a 2-cycle NOP and remember it. */
        if (getenv("VMC10_TRACE_ILLEGAL") && c->illegal_count < 10) fprintf(stderr, "illegal %02X at %04X\n", op, c->ppc);
        c->illegal_count++;
        c->last_illegal = op;
        n = 2;
        break;
    }

    c->cycles += n;
    timer_advance(c, n);
    return (int)(c->cycles - start);
}

/* ---------------- disassembler ---------------- */

enum { M_INH, M_IMM8, M_IMM16, M_DIR, M_IDX, M_EXT, M_REL, M_ILL };

static const char *const mn[256] = {
 "???","NOP","???","???","LSRD","ASLD","TAP","TPA","INX","DEX","CLV","SEV","CLC","SEC","CLI","SEI",
 "SBA","CBA","???","???","???","???","TAB","TBA","???","DAA","???","ABA","???","???","???","???",
 "BRA","BRN","BHI","BLS","BCC","BCS","BNE","BEQ","BVC","BVS","BPL","BMI","BGE","BLT","BGT","BLE",
 "TSX","INS","PULA","PULB","DES","TXS","PSHA","PSHB","PULX","RTS","ABX","RTI","PSHX","MUL","WAI","SWI",
 "NEGA","???","???","COMA","LSRA","???","RORA","ASRA","ASLA","ROLA","DECA","???","INCA","TSTA","???","CLRA",
 "NEGB","???","???","COMB","LSRB","???","RORB","ASRB","ASLB","ROLB","DECB","???","INCB","TSTB","???","CLRB",
 "NEG","???","???","COM","LSR","???","ROR","ASR","ASL","ROL","DEC","???","INC","TST","JMP","CLR",
 "NEG","???","???","COM","LSR","???","ROR","ASR","ASL","ROL","DEC","???","INC","TST","JMP","CLR",
 "SUBA","CMPA","SBCA","SUBD","ANDA","BITA","LDAA","???","EORA","ADCA","ORAA","ADDA","CPX","BSR","LDS","???",
 "SUBA","CMPA","SBCA","SUBD","ANDA","BITA","LDAA","STAA","EORA","ADCA","ORAA","ADDA","CPX","JSR","LDS","STS",
 "SUBA","CMPA","SBCA","SUBD","ANDA","BITA","LDAA","STAA","EORA","ADCA","ORAA","ADDA","CPX","JSR","LDS","STS",
 "SUBA","CMPA","SBCA","SUBD","ANDA","BITA","LDAA","STAA","EORA","ADCA","ORAA","ADDA","CPX","JSR","LDS","STS",
 "SUBB","CMPB","SBCB","ADDD","ANDB","BITB","LDAB","???","EORB","ADCB","ORAB","ADDB","LDD","???","LDX","???",
 "SUBB","CMPB","SBCB","ADDD","ANDB","BITB","LDAB","STAB","EORB","ADCB","ORAB","ADDB","LDD","STD","LDX","STX",
 "SUBB","CMPB","SBCB","ADDD","ANDB","BITB","LDAB","STAB","EORB","ADCB","ORAB","ADDB","LDD","STD","LDX","STX",
 "SUBB","CMPB","SBCB","ADDD","ANDB","BITB","LDAB","STAB","EORB","ADCB","ORAB","ADDB","LDD","STD","LDX","STX",
};

static int dis_mode(uint8_t op)
{
    if (cyc[op] == 0) return M_ILL;
    if (op < 0x20 || (op >= 0x30 && op < 0x60)) return M_INH;
    if (op < 0x30) return M_REL;
    if (op < 0x70) return M_IDX;
    if (op < 0x80) return M_EXT;
    if (op == 0x8D) return M_REL;
    switch ((op >> 4) & 3) {
    case 0: { int lo = op & 0xF;
        if (lo == 3 || lo == 0xC || lo == 0xE) return M_IMM16;
        return M_IMM8; }
    case 1: return M_DIR;
    case 2: return M_IDX;
    default: return M_EXT;
    }
}

int m6803_disasm(m6803 *c, uint16_t pc, char *buf, int len)
{
    uint8_t op = m6803_peek(c, pc);
    uint8_t b1 = m6803_peek(c, (uint16_t)(pc + 1));
    uint8_t b2 = m6803_peek(c, (uint16_t)(pc + 2));
    int mode = dis_mode(op);
    switch (mode) {
    case M_ILL:   snprintf(buf, len, "FCB   $%02X", op); return 1;
    case M_INH:   snprintf(buf, len, "%s", mn[op]); return 1;
    case M_IMM8:  snprintf(buf, len, "%-5s #$%02X", mn[op], b1); return 2;
    case M_IMM16: snprintf(buf, len, "%-5s #$%04X", mn[op], (b1 << 8) | b2); return 3;
    case M_DIR:   snprintf(buf, len, "%-5s <$%02X", mn[op], b1); return 2;
    case M_IDX:   snprintf(buf, len, "%-5s $%02X,X", mn[op], b1); return 2;
    case M_EXT:   snprintf(buf, len, "%-5s $%04X", mn[op], (b1 << 8) | b2); return 3;
    case M_REL:   snprintf(buf, len, "%-5s $%04X", mn[op], (uint16_t)(pc + 2 + (int8_t)b1)); return 2;
    }
    return 1;
}
