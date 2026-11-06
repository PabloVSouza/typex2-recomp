/* Native replacements for hot guest library routines.
 *
 * x2recomp recognises a few statically linked routines by their machine code
 * and emits a call to these instead of lifting them. They work on guest memory
 * through X2P() and must leave the guest state exactly as the original would
 * (the stream/state structures, the guest stack). Loading a KOF XIII fight
 * spends most of its CPU time in zlib's inflate_fast; recompiled, that cost
 * ~100 ms frames on an ARM tablet.
 *
 * x2n_inflate_fast is adapted from zlib 1.2.3 inffast.c, Copyright (C)
 * 1995-2004 Mark Adler, under the zlib license; changed to read and write the
 * guest's z_stream and inflate_state in guest memory. */
#include <string.h>

#include "core.h"

static inline u32 g32(u32 a) { u32 v; memcpy(&v, X2P(a), 4); return v; }
static inline void p32(u32 a, u32 v) { memcpy(X2P(a), &v, 4); }
static inline u16 g16(u32 a) { u16 v; memcpy(&v, X2P(a), 2); return v; }

/* zlib 1.2.3 inflate_fast(z_streamp strm, unsigned start), cdecl. The three
 * messages are the guest addresses of zlib's error strings in this binary.
 *   z_stream: next_in 0, avail_in 4, next_out 12, avail_out 16, msg 24, state 28
 *   state:    mode 0, wsize 40, whave 44, write 48, window 52, hold 56, bits 60,
 *             lencode 76, distcode 80, lenbits 84, distbits 88
 *   code:     {u8 op, u8 bits, u16 val} */
enum { Z_TYPE = 11, Z_BAD = 27 };
void x2n_inflate_fast(X2Cpu *c, u32 msg_far, u32 msg_dist, u32 msg_len) {
    u32 strm = g32(c->esp + 4), start = g32(c->esp + 8);
    u32 st = g32(strm + 28);
    u32 avail_in = g32(strm + 4), avail_out = g32(strm + 16);
    const u8 *in = X2P(g32(strm + 0));
    const u8 *last = in + (avail_in - 5);
    u8 *out = X2P(g32(strm + 12));
    u8 *beg = out - (start - avail_out);
    u8 *end = out + (avail_out - 257);
    u32 wsize = g32(st + 40), whave = g32(st + 44), wnext = g32(st + 48);
    u8 *window = X2P(g32(st + 52));
    u32 hold = g32(st + 56), bits = g32(st + 60);
    u32 lcode = g32(st + 76), dcode = g32(st + 80);
    u32 lmask = (1u << g32(st + 84)) - 1, dmask = (1u << g32(st + 88)) - 1;
    u32 op, len, dist;
    u8 cop, cbits;
    u16 cval;
#define CODE(tab, i) (cop = *X2P((tab) + (i) * 4), cbits = *X2P((tab) + (i) * 4 + 1), cval = g16((tab) + (i) * 4 + 2))
    do {
        if (bits < 15) {
            hold += (u32)*in++ << bits; bits += 8;
            hold += (u32)*in++ << bits; bits += 8;
        }
        CODE(lcode, hold & lmask);
    dolen:
        hold >>= cbits; bits -= cbits;
        op = cop;
        if (op == 0) {
            *out++ = (u8)cval;
        } else if (op & 16) {
            len = cval;
            op &= 15;
            if (op) {
                if (bits < op) { hold += (u32)*in++ << bits; bits += 8; }
                len += hold & ((1u << op) - 1);
                hold >>= op; bits -= op;
            }
            if (bits < 15) {
                hold += (u32)*in++ << bits; bits += 8;
                hold += (u32)*in++ << bits; bits += 8;
            }
            CODE(dcode, hold & dmask);
        dodist:
            hold >>= cbits; bits -= cbits;
            op = cop;
            if (op & 16) {
                dist = cval;
                op &= 15;
                if (bits < op) {
                    hold += (u32)*in++ << bits; bits += 8;
                    if (bits < op) { hold += (u32)*in++ << bits; bits += 8; }
                }
                dist += hold & ((1u << op) - 1);
                hold >>= op; bits -= op;
                op = (u32)(out - beg); /* max distance in output */
                if (dist > op) {       /* see if copy from window */
                    op = dist - op;    /* distance back in window */
                    if (op > whave) {
                        p32(strm + 24, msg_far);
                        p32(st, Z_BAD);
                        break;
                    }
                    const u8 *from = window;
                    if (wnext == 0) { /* very common case */
                        from += wsize - op;
                        if (op < len) { len -= op; do *out++ = *from++; while (--op); from = out - dist; }
                    } else if (wnext < op) { /* wrap around window */
                        from += wsize + wnext - op;
                        op -= wnext;
                        if (op < len) {
                            len -= op;
                            do *out++ = *from++; while (--op);
                            from = window;
                            if (wnext < len) { op = wnext; len -= op; do *out++ = *from++; while (--op); from = out - dist; }
                        }
                    } else { /* contiguous in window */
                        from += wnext - op;
                        if (op < len) { len -= op; do *out++ = *from++; while (--op); from = out - dist; }
                    }
                    while (len > 2) { out[0] = from[0]; out[1] = from[1]; out[2] = from[2]; out += 3; from += 3; len -= 3; }
                    if (len) { *out++ = *from++; if (len > 1) *out++ = *from++; }
                } else {
                    const u8 *from = out - dist; /* copy direct from output */
                    if (dist >= len) { memcpy(out, from, len); out += len; }
                    else {
                        do { out[0] = from[0]; out[1] = from[1]; out[2] = from[2]; out += 3; from += 3; len -= 3; } while (len > 2);
                        if (len) { *out++ = *from++; if (len > 1) *out++ = *from++; }
                    }
                }
            } else if ((op & 64) == 0) { /* 2nd level distance code */
                CODE(dcode, cval + (hold & ((1u << op) - 1)));
                goto dodist;
            } else {
                p32(strm + 24, msg_dist);
                p32(st, Z_BAD);
                break;
            }
        } else if ((op & 64) == 0) { /* 2nd level length code */
            CODE(lcode, cval + (hold & ((1u << op) - 1)));
            goto dolen;
        } else if (op & 32) { /* end-of-block */
            p32(st, Z_TYPE);
            break;
        } else {
            p32(strm + 24, msg_len);
            p32(st, Z_BAD);
            break;
        }
    } while (in < last && out < end);
#undef CODE
    /* return unused bytes (on entry, bits < 8, so in won't go too far back) */
    len = bits >> 3;
    in -= len;
    bits -= len << 3;
    hold &= (1u << bits) - 1;
    p32(strm + 0, (u32)(in - x2_mem));
    p32(strm + 12, (u32)(out - x2_mem));
    p32(strm + 4, (u32)(in < last ? 5 + (last - in) : 5 - (in - last)));
    p32(strm + 16, (u32)(out < end ? 257 + (end - out) : 257 - (out - end)));
    p32(st + 56, hold);
    p32(st + 60, bits);
    c->esp += 4; /* ret */
}

/* void fill(u8 *dst, u32 n, const u8 *val): an unoptimised byte loop (KOF XIII
 * Climax, 0x465fe0). Leaves eax/ecx/edx as the loop does. */
void x2n_fill8(X2Cpu *c) {
    u32 dst = g32(c->esp + 4), n = g32(c->esp + 8), val = g32(c->esp + 12);
    if (n) {
        memset(X2P(dst), *X2P(val), n);
        c->eax = 0;
        c->ecx = dst + n;
        c->edx = dst + n - 1;
    }
    c->esp += 4; /* ret */
}
