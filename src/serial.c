/*
 * serial.c - the MC-10's bit-banged RS-232 port, connected to a TCP peer.
 *
 * MCX BASIC talks to an Emcee file server at 38400 baud, 8-N-1, by toggling
 * port 2 bit 0 (TxD, idle high) and polling port 2 bit 2 (RxD).  We decode
 * the TxD edges by their CPU-cycle timestamps into bytes, and replay
 * received bytes onto RxD bit by bit in CPU time, so the ROM's own timing
 * loops see a real serial waveform.  The bytes travel over a TCP connection,
 * for example to pyDriveWire ("pyDriveWire --accept --port 65504"), which
 * serves Emcee requests.
 */
#include "mc10.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

static double bit_cycles(mc10 *m)
{
    return (double)MC10_CLOCK / (m->serial.baud ? m->serial.baud : 38400);
}

static bool trace(void)
{
    static int t = -1;
    if (t < 0) t = getenv("VMC10_TRACE_SERIAL") != NULL;
    return t;
}

/* ---------------- TCP ---------------- */

static void ser_close(mc10 *m)
{
    if (m->serial.fd >= 0) close(m->serial.fd);
    m->serial.fd = -1;
}

static void ser_connect(mc10 *m)
{
    serialport *s = &m->serial;
    char host[256], port[16] = "65504";
    snprintf(host, sizeof host, "%s", s->target);
    char *colon = strrchr(host, ':');
    if (colon) { snprintf(port, sizeof port, "%s", colon + 1); *colon = 0; }
    if (!host[0]) snprintf(host, sizeof host, "localhost");

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) || !res) return;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) == 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        s->fd = fd;
        mc10_status(m, "SERIAL CONNECTED TO %s:%s", host, port);
    } else if (fd >= 0) close(fd);
    freeaddrinfo(res);
}

static void ser_send(mc10 *m, uint8_t b)
{
    serialport *s = &m->serial;
    if (trace()) fprintf(stderr, "serial: tx %02X '%c'\n", b, b >= 32 && b < 127 ? b : '.');
    if (m->emcee.on) { emcee_byte(m, b); return; }
    if (s->fd < 0) ser_connect(m);
    if (s->fd < 0) return;
    if (send(s->fd, &b, 1, MSG_NOSIGNAL) != 1 && errno != EAGAIN) {
        mc10_status(m, "SERIAL CONNECTION LOST");
        ser_close(m);
    }
}

/* ---------------- TxD decoder ---------------- */

/* Decode complete frames from the recorded TxD transitions. */
static void tx_decode(mc10 *m, uint64_t now)
{
    serialport *s = &m->serial;
    double bc = bit_cycles(m);
    for (;;) {
        if (!s->tx_busy) {
            /* look for a falling edge (start bit) */
            int i;
            for (i = 0; i < s->ntx; i++) if (s->tx_lvl[i] == 0) break;
            if (i == s->ntx) { s->ntx = 0; return; }
            s->tx_start = s->tx_t[i];
            memmove(s->tx_t, s->tx_t + i, (size_t)(s->ntx - i) * sizeof s->tx_t[0]);
            memmove(s->tx_lvl, s->tx_lvl + i, (size_t)(s->ntx - i));
            s->ntx -= i;
            s->tx_busy = true;
        }
        double end = (double)s->tx_start + bc * 9.5;
        if ((double)now < end) return;                /* frame not complete yet */
        int level = 0, k = 0;
        uint8_t byte = 0;
        for (int bit = 0; bit < 8; bit++) {
            double t = (double)s->tx_start + bc * (1.5 + bit);
            while (k < s->ntx && (double)s->tx_t[k] <= t) level = s->tx_lvl[k++];
            byte |= (uint8_t)(level << bit);
        }
        ser_send(m, byte);
        /* drop transitions that belong to this frame */
        while (k < s->ntx && (double)s->tx_t[k] <= end) k++;
        memmove(s->tx_t, s->tx_t + k, (size_t)(s->ntx - k) * sizeof s->tx_t[0]);
        memmove(s->tx_lvl, s->tx_lvl + k, (size_t)(s->ntx - k));
        s->ntx -= k;
        s->tx_busy = false;
    }
}

void serial_txd(mc10 *m, int level, uint64_t cycle)
{
    serialport *s = &m->serial;
    if (!s->enabled || level == s->tx_last) return;
    s->tx_last = level;
    if (s->ntx == SER_MAXEDGES) { tx_decode(m, cycle); if (s->ntx == SER_MAXEDGES) s->ntx = 0; }
    s->tx_t[s->ntx] = cycle;
    s->tx_lvl[s->ntx] = (uint8_t)level;
    s->ntx++;
    tx_decode(m, cycle);
}

/* ---------------- RxD encoder ---------------- */

/* Level of RxD at cycle `now`.  A byte starts when software is polling the
 * line and data is waiting; further queued bytes follow back to back, as a
 * real UART would send them. */
int serial_rxd(mc10 *m, uint64_t now)
{
    serialport *s = &m->serial;
    if (!s->enabled) return 1;
    tx_decode(m, now);
    double bc = bit_cycles(m);
    for (;;) {
        if (!s->rx_busy) {
            if (s->rx_head == s->rx_tail) return 1;
            s->rx_busy = true;
            s->rx_cur = s->rx_q[s->rx_head];
            s->rx_head = (s->rx_head + 1) % SER_RXQ;
            if ((double)now > s->rx_next) s->rx_next = (double)now + bc;   /* start soon */
            s->rx_start = s->rx_next;
        }
        double pos = ((double)now - s->rx_start) / bc;
        if (pos < 0) return 1;                        /* idle before start bit */
        int bit = (int)pos;
        if (bit == 0) return 0;                       /* start */
        if (bit <= 8) return (s->rx_cur >> (bit - 1)) & 1;
        if (bit == 9) return 1;                       /* stop */
        s->rx_busy = false;                           /* next byte, back to back */
        s->rx_next = s->rx_start + bc * 10;
    }
}

/* ---------------- per-frame service ---------------- */

void serial_poll(mc10 *m)
{
    serialport *s = &m->serial;
    if (!s->enabled) return;
    tx_decode(m, m->cpu.cycles);
    if (s->fd < 0) return;
    uint8_t buf[512];
    for (;;) {
        ssize_t n = recv(s->fd, buf, sizeof buf, 0);
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                int nt = (s->rx_tail + 1) % SER_RXQ;
                if (nt == s->rx_head) break;          /* overflow: drop */
                s->rx_q[s->rx_tail] = buf[i];
                s->rx_tail = nt;
                if (trace()) fprintf(stderr, "serial: rx %02X '%c'\n", buf[i], buf[i] >= 32 && buf[i] < 127 ? buf[i] : '.');
            }
            continue;
        }
        if (n == 0) { mc10_status(m, "SERIAL CONNECTION CLOSED"); ser_close(m); }
        break;
    }
}

void serial_rx_push(mc10 *m, const uint8_t *p, size_t n)
{
    serialport *s = &m->serial;
    for (size_t i = 0; i < n; i++) {
        int nt = (s->rx_tail + 1) % SER_RXQ;
        if (nt == s->rx_head) break;
        s->rx_q[s->rx_tail] = p[i];
        s->rx_tail = nt;
    }
}

void serial_init(mc10 *m, const char *target)
{
    serialport *s = &m->serial;
    memset(s, 0, sizeof *s);
    s->fd = -1;
    s->tx_last = 1;
    s->baud = 38400;
    if (!target || !*target) return;
    s->enabled = true;
    snprintf(s->target, sizeof s->target, "%s", target);
    if (!strcmp(target, "builtin")) {                 /* built-in Emcee server */
        m->emcee.on = true;
        return;
    }
    ser_connect(m);
    if (s->fd < 0) mc10_status(m, "SERIAL: CANNOT CONNECT TO %s (WILL RETRY)", target);
}

void serial_shutdown(mc10 *m) { ser_close(m); }
