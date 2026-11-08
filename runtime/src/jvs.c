/* JVS I/O board emulation (what the Type X2 talks to over a serial port).
 *
 * Packet: E0 <node> <len> <data...> <sum>, len counts data + sum,
 * sum = (node + len + data) & 0xFF. 0xE0/0xD0 inside a packet are escaped
 * as D0 (b-1). The master (game) resets the bus, assigns addresses with F1
 * while the sense line is high, then polls switches/coins every frame.
 *
 * Sense line is reported through GetCommModemStatus as MS_CTS_ON: set while
 * our node is unaddressed, cleared once it has an address. */
#include <string.h>

#include "core.h"
#include "jvs.h"

#define SYNC 0xE0
#define ESC 0xD0

static pthread_mutex_t jvs_lock = PTHREAD_MUTEX_INITIALIZER;
static u8 node_addr;         /* 0 = unaddressed */
static u8 rx[512];           /* bytes from the game, being assembled */
static u32 rx_len;
static bool rx_escape;
static u8 tx[4096];          /* bytes for the game to read */
static u32 tx_head, tx_tail;

/* input state written by the platform layer */
static u8 sys_switches;              /* bit7 = test */
static u16 player_switches[2];       /* see jvs.h bit layout */
static u16 coins[2];
static bool coin_prev[2];

void jvs_set_inputs(u8 system, u16 p1, u16 p2, bool coin1, bool coin2) {
    pthread_mutex_lock(&jvs_lock);
    sys_switches = system;
    player_switches[0] = p1;
    player_switches[1] = p2;
    bool c[2] = {coin1, coin2};
    for (int i = 0; i < 2; i++) {
        if (c[i] && !coin_prev[i]) coins[i]++;
        coin_prev[i] = c[i];
    }
    pthread_mutex_unlock(&jvs_lock);
}

static void tx_byte(u8 b) {
    if (tx_tail - tx_head < sizeof tx) tx[tx_tail++ % sizeof tx] = b;
}
static void tx_escaped(u8 b) {
    if (b == SYNC || b == ESC) {
        tx_byte(ESC);
        tx_byte((u8)(b - 1));
    } else {
        tx_byte(b);
    }
}
static void send_packet(const u8 *data, u32 n) {
    u8 len = (u8)(n + 1);
    u8 sum = (u8)(0x00 + len);
    tx_byte(SYNC);
    tx_escaped(0x00);  /* to master */
    tx_escaped(len);
    for (u32 i = 0; i < n; i++) {
        tx_escaped(data[i]);
        sum = (u8)(sum + data[i]);
    }
    tx_escaped(sum);
}

static const char kIdent[] = "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10";

/* Handle one packet's command list addressed to us; builds the reply. */
static void handle(const u8 *d, u32 n) {
    u8 out[1024];
    u32 o = 0;
    out[o++] = 0x01; /* status: normal */
    u32 i = 0;
    while (i < n) {
        u8 cmd = d[i++];
        switch (cmd) {
            case 0xF0: /* reset (broadcast, handled before addressing) */
                i++;
                node_addr = 0;
                return;
            case 0xF1: /* set address */
                node_addr = d[i++];
                out[o++] = 0x01;
                x2_log("jvs: node address set to %u", node_addr);
                break;
            case 0x10: /* read identity */
                out[o++] = 0x01;
                memcpy(out + o, kIdent, sizeof kIdent);
                o += sizeof kIdent;
                break;
            case 0x11: out[o++] = 0x01; out[o++] = 0x13; break; /* command revision 1.3 */
            case 0x12: out[o++] = 0x01; out[o++] = 0x30; break; /* JVS revision 3.0 */
            case 0x13: out[o++] = 0x01; out[o++] = 0x10; break; /* comm version 1.0 */
            case 0x14: { /* feature check */
                static const u8 feat[] = {
                    0x01, 2, 13, 0,   /* switch input: 2 players, 13 switches */
                    0x02, 2, 0, 0,    /* coin input: 2 slots */
                    0x03, 8, 10, 0,   /* analog input: 8 channels, 10 bits */
                    0x12, 8, 0, 0,    /* general purpose output: 8 */
                    0x00,
                };
                out[o++] = 0x01;
                memcpy(out + o, feat, sizeof feat);
                o += sizeof feat;
                break;
            }
            case 0x15: /* main board ID: string follows, NUL terminated */
                while (i < n && d[i]) i++;
                i++;
                out[o++] = 0x01;
                break;
            case 0x20: { /* read switches: players, bytes per player */
                u8 np = d[i++], nb = d[i++];
                out[o++] = 0x01;
                out[o++] = sys_switches;
                for (u8 p = 0; p < np; p++)
                    for (u8 b = 0; b < nb; b++) {
                        u16 v = p < 2 ? player_switches[p] : 0;
                        out[o++] = b == 0 ? (u8)(v >> 8) : b == 1 ? (u8)v : 0;
                    }
                break;
            }
            case 0x21: { /* read coins */
                u8 ns = d[i++];
                out[o++] = 0x01;
                for (u8 s = 0; s < ns; s++) {
                    u16 v = s < 2 ? coins[s] & 0x3FFF : 0;
                    out[o++] = (u8)(v >> 8);
                    out[o++] = (u8)v;
                }
                break;
            }
            case 0x22: { /* read analog */
                u8 nc = d[i++];
                out[o++] = 0x01;
                for (u8 ch = 0; ch < nc; ch++) { out[o++] = 0x80; out[o++] = 0x00; }
                break;
            }
            case 0x30: { /* decrease coins */
                u8 s = d[i++];
                u16 amt = (u16)(d[i] << 8 | d[i + 1]);
                i += 2;
                if (s >= 1 && s <= 2) coins[s - 1] = coins[s - 1] > amt ? coins[s - 1] - amt : 0;
                out[o++] = 0x01;
                break;
            }
            case 0x31: { /* increase coins */
                u8 s = d[i++];
                u16 amt = (u16)(d[i] << 8 | d[i + 1]);
                i += 2;
                if (s >= 1 && s <= 2) coins[s - 1] += amt;
                out[o++] = 0x01;
                break;
            }
            case 0x32: /* general purpose output (lamps): n bytes */
                i += 1 + d[i];
                out[o++] = 0x01;
                break;
            case 0x2F: /* retransmit */
                out[o++] = 0x01;
                break;
            default:
                x2_log("jvs: unknown command %02x", cmd);
                out[0] = 0x02; /* unknown command */
                send_packet(out, o);
                return;
        }
        if (o > sizeof out - 128) break;
    }
    send_packet(out, o);
}

static void process_packet(const u8 *p, u32 n) {
    /* p: node, len, data..., sum (sync already stripped) */
    if (n < 3) return;
    u8 nodeb = p[0], len = p[1];
    if ((u32)len + 2 != n) return;
    u8 sum = 0;
    for (u32 i = 0; i + 1 < n; i++) sum = (u8)(sum + p[i]);
    if (sum != p[n - 1]) {
        x2_log("jvs: checksum error");
        return;
    }
    const u8 *data = p + 2;
    u32 dn = len - 1u;
    if (nodeb == 0xFF) { /* broadcast: reset, or address assignment for the unaddressed node */
        if (dn >= 2 && data[0] == 0xF0) {
            node_addr = 0;
            X2_TRACE("jvs: bus reset");
            return;
        }
        if (dn >= 2 && data[0] == 0xF1 && node_addr == 0) handle(data, dn);
        return;
    }
    if (nodeb == 0x00 && !node_addr && dn >= 1) {
        /* BlazBlue CT first talks to a controller on the I/O board itself
         * (address 0, commands 0-8: e.g. 05 <u16 ms> set a timeout, 04, 08)
         * before it starts JVS on the same port; it only checks the status
         * byte and skips 1 or 2 report bytes per command. */
        static const u8 report_len[9] = {1, 2, 2, 1, 1, 1, 2, 1, 2};
        u8 out[8] = {0x01};
        u32 o = 1, nrep = data[0] < 9 ? report_len[data[0]] : 1;
        for (u32 k = 0; k < nrep; k++) out[o++] = 0x01;
        X2_TRACE("jvs: I/O controller command %02x acknowledged", data[0]);
        send_packet(out, o);
        return;
    }
    if (nodeb == 0x00 && node_addr) {
        /* KOF XII's "JVS Libraly 1.06" sends `01 02` to node 0 at the start of
         * every poll cycle and waits for a reply; acknowledge it. */
        static const u8 ack[] = {0x01, 0x01};
        X2_TRACE("jvs: node-0 packet (%u bytes) acknowledged", dn);
        send_packet(ack, sizeof ack);
        return;
    }
    if (node_addr && nodeb == node_addr) handle(data, dn);
}

void jvs_write(const u8 *b, u32 n) {
    if (x2_log_level >= 2) {
        char hex[3 * 64 + 1] = {0};
        for (u32 i = 0; i < n && i < 64; i++) snprintf(hex + 3 * i, 4, "%02x ", b[i]);
        x2_log("jvs <- %s(sense=%d)", hex, node_addr == 0);
    }
    pthread_mutex_lock(&jvs_lock);
    for (u32 i = 0; i < n; i++) {
        u8 v = b[i];
        if (v == SYNC) {
            rx_len = 0;
            rx_escape = false;
            continue;
        }
        if (v == ESC) {
            rx_escape = true;
            continue;
        }
        if (rx_escape) {
            v = (u8)(v + 1);
            rx_escape = false;
        }
        if (rx_len < sizeof rx) rx[rx_len++] = v;
        if (rx_len >= 2 && rx_len == (u32)rx[1] + 2) {
            process_packet(rx, rx_len);
            rx_len = 0;
        }
    }
    pthread_mutex_unlock(&jvs_lock);
}

u32 jvs_read(u8 *out, u32 n) {
    pthread_mutex_lock(&jvs_lock);
    u32 k = 0;
    while (k < n && tx_head != tx_tail) out[k++] = tx[tx_head++ % sizeof tx];
    pthread_mutex_unlock(&jvs_lock);
    if (x2_log_level >= 2 && k) {
        char hex[3 * 80 + 1] = {0};
        for (u32 i = 0; i < k && i < 80; i++) snprintf(hex + 3 * i, 4, "%02x ", out[i]);
        x2_log("jvs -> %s(%u of %u)", hex, k, n);
    }
    return k;
}

u32 jvs_pending(void) {
    pthread_mutex_lock(&jvs_lock);
    u32 n = tx_tail - tx_head;
    pthread_mutex_unlock(&jvs_lock);
    return n;
}

bool jvs_sense(void) { return node_addr == 0; }
