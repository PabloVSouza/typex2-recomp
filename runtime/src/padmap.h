/* Gamepad face/shoulder buttons → cabinet buttons, shared by both front ends.
 *
 * The default follows FinalBurn Neo's Neo Geo layout for the "Modern" RetroPad
 * (retro_input.cpp: Neo Geo "fire 1-4" on Y B X A, macros AB CD ABC BCD on
 * R2 R1 L2 L1): left face A, bottom B, top C, right D, R2 A+B, R1 C+D,
 * L2 A+B+C, L1 B+C+D, so a RetroArch remap made for FBNeo works unchanged.
 * L3 / R3 do nothing (frontends often use them as hotkeys). Coin
 * (Select / Back) held for X2_PAD_HOLD_SECONDS presses the cabinet's Test
 * switch.
 *
 * Other games get their own default (x2_game->id): SSF4 AE punches on Y X R1,
 * kicks on B A R2, L1 / L2 = three punches / kicks; BlazBlue CS A B C on
 * Y X R1, D on B, A = B+C, R2 = A+B, L1 = A+B+C, L2 = A+B+C+D.
 *
 * X2_PAD overrides the map as "button=keys" pairs, e.g.
 *   X2_PAD=south=b,east=ab,west=d,north=cd,l1=c,r1=bcd,l2=a,r2=abc,l3=t
 * buttons: south east west north l1 r1 l2 r2 l3 r3; keys: any of a b c d (KOF's
 * A-D), 1-6 (cabinet SHOT1-6, any game), t (Test),
 * v (Service), or "-" for nothing. Positions are physical: south = bottom
 * face button (RetroPad B, Xbox A), east = right (RetroPad A), west = left
 * (RetroPad Y), north = top (RetroPad X). The letters are KOF's A-D, which sit
 * on cabinet SHOT1 (A), SHOT4 (B), SHOT2 (C), SHOT5 (D); see jvs.h. */
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
/* X2_INPUT="frame:keys,frame:keys,..." scripted input for automated tests; each
 * entry holds from its frame until the next. keys: c coin 1, s start 1, u d l r,
 * a b x y 5 6 = cabinet SHOT1-6 (KOF: A C - B D -), v service, t test (returned),
 * p draw capture, '-' nothing; upper case C S U D L R A B X Y act for player 2.
 * Adds to *p1 / *coin1 / *p2 / *coin2. */
bool x2_script_input(uint64_t frame, uint16_t *p1, bool *coin1, uint16_t *p2, bool *coin2);
