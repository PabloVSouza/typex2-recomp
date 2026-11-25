/* iDmacDrv32.dll: the driver of the I/O board on NESiCAxLive Type X2 cabinets (Goketsuji
 * Ichizoku Matsuri Senzo Kuyou). The game opens it by ordinal and polls 32-bit registers:
 *   0x0400            read once at open
 *   0x4000 / 0x4004   status words (bits 16-23 >= 3: the game acknowledges by writing 0)
 *   0x4120-0x412c, 0x41a0-0x41ac   input words; 0x4100-0x410c, 0x4180-0x418c   outputs
 *   0x4140 / 0x4144, 0x41c0 / 0x41c4   coin counters (the game writes minus what it took)
 * Every call returns a Win32 error code (0 = success) and writes a status through its last
 * argument. */
#include <stdlib.h>
#include <string.h>

#include "../core.h"
#include "../jvs.h"

static u32 regs[0x4400 / 4]; /* 0x0000-0x43ff, word-indexed */
static bool trace = false;

static u32 *reg(u32 r) { return r < sizeof regs && !(r & 3) ? &regs[r / 4] : NULL; }

/* a node's input word from the JVS state. Each node is one player's panel (Goketsuji reads player
   2 from node 2): JVS switch bits 15 (start) .. 2 (button 8) at these bit positions (a second
   player on the same node would be one bit higher; unused); the test switch at bit 6, node 1 */
static u32 input_word(int node) {
    static const u8 pos[14] = {4, 2, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30};
    u8 sys;
    u16 p[2];
    jvs_get_switches(&sys, &p[0], &p[1]);
    u32 w = node == 0 && sys & JVS_SYS_TEST ? 0x40u : 0;
    for (int k = 0; k < 14; k++)
        if (p[node] & (1u << (15 - k))) w |= 1u << pos[k];
    return w;
}
/* (index, &handle, &status) */
static void Open(X2Cpu *c) {
    if (ARG(1)) WR32(ARG(1), 1);
    if (ARG(2)) WR32(ARG(2), 0);
    trace = getenv("X2_TRACE") != NULL;
    /* both nodes linked (bytes 0 and 2 set; byte 2 >= 3 flags the status as new, and the game
       writes 0 back once it has taken it) */
    *reg(0x4000) = 0x00030001;
    *reg(0x4004) = 0x00030001;
    x2_log("idmac: board %u opened", ARG(0));
    RET(0);
}
/* (handle, &status) */
static void Close(X2Cpu *c) {
    if (ARG(1)) WR32(ARG(1), 0);
    RET(0);
}
/* (handle, register, &value, &status) */
static void RegisterRead(X2Cpu *c) {
    u32 r = ARG(1), *p = reg(r), v = p ? *p : 0;
    if (r == 0x4120 || r == 0x41a0) v = input_word(r == 0x41a0);
    else if (r == 0x4140 || r == 0x41c0) v = jvs_get_coins(r == 0x41c0); /* each node's first coin slot */
    if (trace) x2_log("idmac: read %04x -> %08x", r, v);
    if (ARG(2)) WR32(ARG(2), v);
    if (ARG(3)) WR32(ARG(3), 0);
    RET(p ? 0 : 87); /* ERROR_INVALID_PARAMETER */
}
/* (handle, register, value, &status) */
static void RegisterWrite(X2Cpu *c) {
    u32 r = ARG(1), *p = reg(r);
    if (trace) x2_log("idmac: write %04x <- %08x", r, ARG(2));
    if (r == 0x4140 || r == 0x41c0) jvs_add_coins(r == 0x41c0, (s32)ARG(2)); /* minus the coins taken */
    else if (p) *p = ARG(2);
    if (ARG(3)) WR32(ARG(3), 0);
    RET(p ? 0 : 87);
}
const X2Hle x2_hle_idmac[] = {
    {"#1", Open, 3},
    {"#2", Close, 2},
    {"#5", RegisterRead, 4},
    {"#6", RegisterWrite, 4},
    {NULL, NULL, 0},
};
