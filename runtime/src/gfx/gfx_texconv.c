/* Texture data conversions shared by the backends: DXT (S3TC) decoding for GPUs
 * without it, and D3D's 16-bit colour formats expanded to BGRA8. */
#include <stdlib.h>
#include <string.h>

#include "gfx_cmd.h"

static void rgb565(u16 c, u8 *o) {
    o[0] = (u8)((c >> 11 & 31) * 255 / 31);
    o[1] = (u8)((c >> 5 & 63) * 255 / 63);
    o[2] = (u8)((c & 31) * 255 / 31);
}

/* one 4x4 colour block (DXT1 layout); dxt1 enables the 3-colour + transparent mode */
static void dxt_color_block(const u8 *b, bool dxt1, u8 out[16][4]) {
    u16 c0 = (u16)(b[0] | b[1] << 8), c1 = (u16)(b[2] | b[3] << 8);
    u8 pal[4][4];
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
    for (int k = 0; k < 3; k++) {
        if (c0 > c1 || !dxt1) {
            pal[2][k] = (u8)((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = (u8)((pal[0][k] + 2 * pal[1][k]) / 3);
        } else {
            pal[2][k] = (u8)((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
    }
    if (dxt1 && c0 <= c1) pal[3][3] = 0;
    u32 bits = (u32)b[4] | (u32)b[5] << 8 | (u32)b[6] << 16 | (u32)b[7] << 24;
    for (int i = 0; i < 16; i++) memcpy(out[i], pal[bits >> (2 * i) & 3], 4);
}

/* decode w x h texels of packed block rows into RGBA8 */
u8 *gfx_dxt_decode(u32 fmt, const u8 *src, u32 w, u32 h) {
    u32 bw = (w + 3) / 4, bh = (h + 3) / 4, bb = fmt == GFX_FMT_DXT1 ? 8 : 16;
    u8 *out = malloc((size_t)w * h * 4);
    for (u32 by = 0; by < bh; by++)
        for (u32 bx = 0; bx < bw; bx++) {
            const u8 *b = src + ((size_t)by * bw + bx) * bb;
            u8 px[16][4];
            dxt_color_block(fmt == GFX_FMT_DXT1 ? b : b + 8, fmt == GFX_FMT_DXT1, px);
            if (fmt == GFX_FMT_DXT3) {
                for (int i = 0; i < 16; i++) px[i][3] = (u8)((b[i / 2] >> (4 * (i & 1)) & 15) * 17);
            } else if (fmt == GFX_FMT_DXT5) {
                u8 a[8] = {b[0], b[1]};
                for (int k = 2; k < 8; k++)
                    a[k] = a[0] > a[1] ? (u8)(((8 - k) * a[0] + (k - 1) * a[1]) / 7)
                           : k < 6     ? (u8)(((6 - k) * a[0] + (k - 1) * a[1]) / 5)
                                       : (k == 6 ? 0 : 255);
                u64 ab = 0;
                for (int k = 0; k < 6; k++) ab |= (u64)b[2 + k] << (8 * k);
                for (int i = 0; i < 16; i++) px[i][3] = a[ab >> (3 * i) & 7];
            }
            for (int i = 0; i < 16; i++) {
                u32 x = bx * 4 + (u32)(i & 3), y = by * 4 + (u32)(i >> 2);
                if (x < w && y < h) memcpy(out + ((size_t)y * w + x) * 4, px[i], 4);
            }
        }
    return out;
}

void gfx_expand16(u32 f, const u8 *src, u32 pitch, u32 w, u32 h, u8 *dst) {
    for (u32 y = 0; y < h; y++) {
        const u16 *s = (const u16 *)(src + (size_t)y * pitch);
        u8 *d = dst + (size_t)y * w * 4;
        for (u32 x = 0; x < w; x++, d += 4) {
            u16 c = s[x];
            u32 r, g, b, a;
            if (f == GFX_FMT_B5G6R5) {
                r = (c >> 11 & 31) * 255 / 31;
                g = (c >> 5 & 63) * 255 / 63;
                b = (c & 31) * 255 / 31;
                a = 255;
            } else if (f == GFX_FMT_BGR5A1) {
                r = (c >> 10 & 31) * 255 / 31;
                g = (c >> 5 & 31) * 255 / 31;
                b = (c & 31) * 255 / 31;
                a = c >> 15 ? 255 : 0;
            } else {
                r = (c >> 8 & 15) * 17;
                g = (c >> 4 & 15) * 17;
                b = (c & 15) * 17;
                a = (c >> 12 & 15) * 17;
            }
            d[0] = (u8)b;
            d[1] = (u8)g;
            d[2] = (u8)r;
            d[3] = (u8)a;
        }
    }
}
bool gfx_dxt1_has_alpha(const u8 *b, u32 bytes) {
    for (u32 o = 0; o + 8 <= bytes; o += 8) {
        u32 c0 = b[o] | b[o + 1] << 8, c1 = b[o + 2] | b[o + 3] << 8;
        if (c0 > c1) continue;
        u32 idx = b[o + 4] | b[o + 5] << 8 | b[o + 6] << 16 | (u32)b[o + 7] << 24;
        for (int i = 0; i < 16; i++)
            if ((idx >> (2 * i) & 3) == 3) return true;
    }
    return false;
}
u8 *gfx_rgba_to_565(u8 *rgba, u32 n) { /* in place: the result fits the first half */
    u16 *o = (u16 *)rgba;
    for (u32 i = 0; i < n; i++) {
        const u8 *p = rgba + i * 4;
        o[i] = (u16)((p[0] * 31 + 127) / 255 << 11 | (p[1] * 63 + 127) / 255 << 5 | (p[2] * 31 + 127) / 255);
    }
    return rgba;
}
