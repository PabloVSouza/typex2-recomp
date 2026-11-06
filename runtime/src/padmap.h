/* Gamepad face/shoulder buttons → cabinet buttons, shared by both front ends.
 *
 * The default follows FinalBurn Neo's Neo Geo layout for the "Modern" RetroPad
 * (KOF on a pad): bottom row A B, top row C D, R2 A+B, R1 C+D, L2 A+B+C,
 * L1 B+C+D. X2_PAD overrides it as "button=keys" pairs, e.g.
 *   X2_PAD=south=b,east=ab,west=d,north=cd,l1=c,r1=bcd,l2=a,r2=abc
 * buttons: south east west north l1 r1 l2 r2; keys: any of a b c d 5 6, or "-"
 * for nothing. Positions are physical: south = bottom face button (RetroPad
 * B, Xbox A), east = right (RetroPad A), west = left (RetroPad Y), north = top
 * (RetroPad X). */
#pragma once
#include <stdint.h>

enum { X2P_SOUTH, X2P_EAST, X2P_WEST, X2P_NORTH, X2P_L1, X2P_R1, X2P_L2, X2P_R2, X2P_COUNT };

/* JVS player bits for each button (read X2_PAD on first call) */
const uint16_t *x2_padmap(void);
/* "A+B" style label of a JVS bit set ("-" if none), for input descriptors */
void x2_padmap_label(uint16_t bits, char out[16]);
