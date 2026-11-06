/* Gamepad face/shoulder buttons → cabinet buttons, shared by both front ends.
 *
 * The default follows FinalBurn Neo's Neo Geo layout for the "Modern" RetroPad
 * (KOF on a pad): bottom row A B, top row C D, R2 A+B, R1 C+D, L2 A+B+C,
 * L1 B+C+D. L3 / R3 do nothing (frontends often use them as hotkeys). Coin
 * (Select / Back) held for X2_PAD_HOLD_SECONDS presses the cabinet's Test
 * switch.
 *
 * X2_PAD overrides the map as "button=keys" pairs, e.g.
 *   X2_PAD=south=b,east=ab,west=d,north=cd,l1=c,r1=bcd,l2=a,r2=abc,l3=t
 * buttons: south east west north l1 r1 l2 r2 l3 r3; keys: any of a b c d 5 6,
 * t (Test), v (Service), or "-" for nothing. Positions are physical: south =
 * bottom face button (RetroPad B, Xbox A), east = right (RetroPad A), west =
 * left (RetroPad Y), north = top (RetroPad X). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { X2P_SOUTH, X2P_EAST, X2P_WEST, X2P_NORTH, X2P_L1, X2P_R1, X2P_L2, X2P_R2, X2P_L3, X2P_R3, X2P_COUNT };

/* pseudo player bit for the Test switch (a system input, not a player one);
 * bit 0 is unused by the JVS player word */
#define X2_PAD_TEST 0x0001
#define X2_PAD_HOLD_SECONDS 2.0

/* JVS player bits for each button (read X2_PAD on first call) */
const uint16_t *x2_padmap(void);
/* "A+B" style label of a bit set ("-" if none), for input descriptors */
void x2_padmap_label(uint16_t bits, char out[16]);
/* true once coin has been held long enough to mean Test (one slot per player) */
bool x2_padmap_hold_test(int slot, bool coin_down);
