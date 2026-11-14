/*
 * debugger.c - simple terminal monitor (replaces VMC10's "cheap debugger").
 */
#include "debugger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void show_regs(mc10 *m)
{
    m6803 *c = &m->cpu;
    char dis[64];
    m6803_disasm(c, c->pc, dis, sizeof dis);
    printf("PC=%04X A=%02X B=%02X X=%04X SP=%04X CC=%c%c%c%c%c%c  cyc=%llu  | %04X  %s\n",
           c->pc, c->a, c->b, c->x, c->sp,
           c->cc & CC_H ? 'H' : '.', c->cc & CC_I ? 'I' : '.', c->cc & CC_N ? 'N' : '.',
           c->cc & CC_Z ? 'Z' : '.', c->cc & CC_V ? 'V' : '.', c->cc & CC_C ? 'C' : '.',
           (unsigned long long)c->cycles, c->pc, dis);
}

static void disasm(mc10 *m, uint16_t a, int n)
{
    char dis[64];
    for (int i = 0; i < n; i++) {
        int len = m6803_disasm(&m->cpu, a, dis, sizeof dis);
        printf("%04X  ", a);
        for (int k = 0; k < 3; k++)
            if (k < len) printf("%02X ", m6803_peek(&m->cpu, (uint16_t)(a + k)));
            else printf("   ");
        printf(" %s\n", dis);
        a = (uint16_t)(a + len);
    }
}

static void dump(mc10 *m, uint16_t a, int n)
{
    for (int i = 0; i < n; i += 16) {
        printf("%04X ", (uint16_t)(a + i));
        for (int k = 0; k < 16; k++) printf(" %02X", m6803_peek(&m->cpu, (uint16_t)(a + i + k)));
        printf("  ");
        for (int k = 0; k < 16; k++) {
            uint8_t b = m6803_peek(&m->cpu, (uint16_t)(a + i + k));
            putchar(b >= 0x20 && b < 0x7F ? b : '.');
        }
        putchar('\n');
    }
}

static bool num(const char *s, unsigned *out)
{
    if (!s) return false;
    char *end;
    int base = 16;
    if (*s == '$') s++;
    else if (*s == '#') { s++; base = 10; }
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    unsigned long v = strtoul(s, &end, base);
    if (end == s) return false;
    *out = (unsigned)v;
    return true;
}

static void help(void)
{
    puts("commands (numbers are hex; prefix # for decimal):\n"
         "  r                 show registers\n"
         "  s [n]             step n instructions\n"
         "  c                 continue (close debugger)\n"
         "  d [addr] [n]      disassemble\n"
         "  m addr [n]        memory dump\n"
         "  w addr byte...    write memory\n"
         "  pc addr           set program counter\n"
         "  b addr | b        add breakpoint | list breakpoints\n"
         "  bc [addr]         clear breakpoint(s)\n"
         "  bload file addr   load binary file into memory\n"
         "  csavem file name start end exec   save memory as .C10 tape\n"
         "  reset | hard      soft / power-on reset\n"
         "  q                 quit the emulator");
}

bool debugger_run(mc10 *m)
{
    char line[512];
    uint16_t daddr = m->cpu.pc;
    if (m->break_hit) printf("\n*** breakpoint at $%04X\n", m->cpu.pc);
    else printf("\n*** VMC10 debugger (h for help)\n");
    m->break_hit = false;
    show_regs(m);
    for (;;) {
        printf("dbg> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) return true;
        char *argv[8] = { 0 };
        int argc = 0;
        for (char *t = strtok(line, " \t\r\n"); t && argc < 8; t = strtok(NULL, " \t\r\n")) argv[argc++] = t;
        if (!argc) continue;
        unsigned a, b, e, x;
        const char *cmd = argv[0];
        if (!strcmp(cmd, "h") || !strcmp(cmd, "?") || !strcmp(cmd, "help")) help();
        else if (!strcmp(cmd, "r")) show_regs(m);
        else if (!strcmp(cmd, "c") || !strcmp(cmd, "g")) { m->bp_skip = true; return true; }
        else if (!strcmp(cmd, "q")) return false;
        else if (!strcmp(cmd, "s")) {
            unsigned n = 1;
            if (argc > 1) num(argv[1], &n);
            for (unsigned i = 0; i < n; i++) m6803_step(&m->cpu);
            show_regs(m);
            daddr = m->cpu.pc;
        }
        else if (!strcmp(cmd, "d")) {
            unsigned n = 16;
            if (argc > 1 && num(argv[1], &a)) daddr = (uint16_t)a;
            if (argc > 2) num(argv[2], &n);
            disasm(m, daddr, (int)n);
        }
        else if (!strcmp(cmd, "m") && argc > 1 && num(argv[1], &a)) {
            unsigned n = 64;
            if (argc > 2) num(argv[2], &n);
            dump(m, (uint16_t)a, (int)n);
        }
        else if (!strcmp(cmd, "w") && argc > 2 && num(argv[1], &a)) {
            for (int i = 2; i < argc; i++)
                if (num(argv[i], &b)) m6803_wr(&m->cpu, (uint16_t)(a + i - 2), (uint8_t)b);
        }
        else if (!strcmp(cmd, "pc") && argc > 1 && num(argv[1], &a)) { m->cpu.pc = (uint16_t)a; show_regs(m); }
        else if (!strcmp(cmd, "b")) {
            if (argc > 1 && num(argv[1], &a) && m->nbreak < 16) m->breakpoints[m->nbreak++] = (uint16_t)a;
            for (int i = 0; i < m->nbreak; i++) printf("  bp %d: $%04X\n", i, m->breakpoints[i]);
        }
        else if (!strcmp(cmd, "bc")) {
            if (argc > 1 && num(argv[1], &a)) {
                for (int i = 0; i < m->nbreak; i++)
                    if (m->breakpoints[i] == a) m->breakpoints[i--] = m->breakpoints[--m->nbreak];
            } else m->nbreak = 0;
        }
        else if (!strcmp(cmd, "bload") && argc > 2 && num(argv[2], &a)) {
            if (!mc10_load_binary(m, argv[1], (uint16_t)a)) printf("cannot read %s\n", argv[1]);
        }
        else if (!strcmp(cmd, "csavem") && argc > 5 && num(argv[3], &a) && num(argv[4], &e) && num(argv[5], &x)) {
            if (!mc10_csavem(m, argv[1], argv[2], (uint16_t)a, (uint16_t)e, (uint16_t)x))
                printf("cannot write %s\n", argv[1]);
        }
        else if (!strcmp(cmd, "reset")) { mc10_reset(m, false); show_regs(m); }
        else if (!strcmp(cmd, "hard")) { mc10_reset(m, true); show_regs(m); }
        else printf("? (h for help)\n");
    }
}
