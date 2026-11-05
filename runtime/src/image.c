/* Image decoding for D3DX: PNG (all color types, 1-16 bit, interlaced) via zlib. */
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "image.h"

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* unfilter one (sub)image in place; returns false on bad filter type */
static bool unfilter(u8 *data, u32 w, u32 h, u32 bpp_bits, u32 channels, u8 *out_rows, u32 stride) {
    u32 bpp = (bpp_bits * channels + 7) / 8;
    u32 row = (w * bpp_bits * channels + 7) / 8;
    u8 *prev = calloc(1, row);
    const u8 *in = data;
    for (u32 y = 0; y < h; y++) {
        u8 ft = *in++;
        u8 *cur = out_rows + (size_t)y * stride;
        memcpy(cur, in, row);
        in += row;
        for (u32 i = 0; i < row; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], cc = i >= bpp ? prev[i - bpp] : 0;
            switch (ft) {
                case 0: break;
                case 1: cur[i] = (u8)(cur[i] + a); break;
                case 2: cur[i] = (u8)(cur[i] + b); break;
                case 3: cur[i] = (u8)(cur[i] + ((a + b) >> 1)); break;
                case 4: cur[i] = (u8)(cur[i] + paeth(a, b, cc)); break;
                default: free(prev); return false;
            }
        }
        memcpy(prev, cur, row);
    }
    free(prev);
    return true;
}

static u32 sample(const u8 *row, u32 x, u32 bits, u32 ch, u32 c) {
    if (bits == 8) return row[x * ch + c];
    if (bits == 16) return row[(x * ch + c) * 2]; /* keep the high byte */
    u32 bit = (x * ch + c) * bits;
    u32 v = (row[bit / 8] >> (8 - bits - bit % 8)) & ((1u << bits) - 1);
    return v * 255 / ((1u << bits) - 1);
}

u8 *x2_decode_png(const u8 *p, size_t n, u32 *ow, u32 *oh, bool *has_alpha) {
    if (n < 33 || memcmp(p, "\x89PNG\r\n\x1a\n", 8)) return NULL;
    u32 w = 0, h = 0, bits = 0, ctype = 0, interlace = 0;
    u8 pal[256][4];
    memset(pal, 255, sizeof pal);
    u32 npal = 0;
    u8 trns_gray[2] = {0}, trns_rgb[6] = {0};
    bool has_trns = false;
    u8 *idat = NULL;
    size_t idat_n = 0;
    for (size_t off = 8; off + 12 <= n;) {
        u32 len = be32(p + off);
        const u8 *type = p + off + 4, *d = p + off + 8;
        if (off + 12 + len > n) break;
        if (!memcmp(type, "IHDR", 4)) {
            w = be32(d); h = be32(d + 4); bits = d[8]; ctype = d[9]; interlace = d[12];
        } else if (!memcmp(type, "PLTE", 4)) {
            npal = len / 3;
            for (u32 i = 0; i < npal && i < 256; i++) { pal[i][0] = d[3 * i]; pal[i][1] = d[3 * i + 1]; pal[i][2] = d[3 * i + 2]; }
        } else if (!memcmp(type, "tRNS", 4)) {
            has_trns = true;
            if (ctype == 3) for (u32 i = 0; i < len && i < 256; i++) pal[i][3] = d[i];
            else if (ctype == 0 && len >= 2) memcpy(trns_gray, d, 2);
            else if (ctype == 2 && len >= 6) memcpy(trns_rgb, d, 6);
        } else if (!memcmp(type, "IDAT", 4)) {
            idat = realloc(idat, idat_n + len);
            memcpy(idat + idat_n, d, len);
            idat_n += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        off += 12 + len;
    }
    if (!w || !h || !idat || w > 16384 || h > 16384) { free(idat); return NULL; }
    u32 ch = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : 4;
    /* inflate */
    size_t row = ((size_t)w * bits * ch + 7) / 8;
    size_t raw_cap = (row + 1) * h + h * 8 + 64;
    u8 *raw = malloc(raw_cap);
    z_stream zs = {0};
    inflateInit(&zs);
    zs.next_in = idat;
    zs.avail_in = (uInt)idat_n;
    zs.next_out = raw;
    zs.avail_out = (uInt)raw_cap;
    int zr = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    free(idat);
    if (zr != Z_STREAM_END && zr != Z_OK && zr != Z_BUF_ERROR) { free(raw); return NULL; }

    /* unfilter into a full-size buffer of packed rows (per pass for Adam7) */
    u8 *rows = calloc(1, row * h);
    u8 *rgba = malloc((size_t)w * h * 4);
    static const u32 sx[7] = {0, 4, 0, 2, 0, 1, 0}, sy[7] = {0, 0, 4, 0, 2, 0, 1};
    static const u32 dx[7] = {8, 8, 4, 4, 2, 2, 1}, dy[7] = {8, 8, 8, 4, 4, 2, 2};
    int passes = interlace ? 7 : 1;
    u8 *src = raw;
    for (int pass = 0; pass < passes; pass++) {
        u32 pw = interlace ? (w + dx[pass] - 1 - sx[pass]) / dx[pass] : w;
        u32 ph = interlace ? (h + dy[pass] - 1 - sy[pass]) / dy[pass] : h;
        if (interlace && (w <= sx[pass] || h <= sy[pass])) continue;
        size_t prow = ((size_t)pw * bits * ch + 7) / 8;
        u8 *tmp = malloc(prow * ph + 1);
        if (!unfilter(src, pw, ph, bits, ch, tmp, (u32)prow)) { free(tmp); free(rows); free(rgba); free(raw); return NULL; }
        src += (prow + 1) * ph;
        for (u32 y = 0; y < ph; y++) {
            const u8 *r = tmp + y * prow;
            for (u32 x = 0; x < pw; x++) {
                u32 X = interlace ? sx[pass] + x * dx[pass] : x, Y = interlace ? sy[pass] + y * dy[pass] : y;
                u8 *o = rgba + ((size_t)Y * w + X) * 4;
                switch (ctype) {
                    case 0: {
                        u32 g = sample(r, x, bits, 1, 0);
                        o[0] = o[1] = o[2] = (u8)g;
                        o[3] = 255;
                        if (has_trns && bits <= 8 && g == (u32)trns_gray[1] * 255 / ((1u << bits) - 1)) o[3] = 0;
                        break;
                    }
                    case 2:
                        o[0] = (u8)sample(r, x, bits, 3, 0); o[1] = (u8)sample(r, x, bits, 3, 1); o[2] = (u8)sample(r, x, bits, 3, 2);
                        o[3] = has_trns && bits == 8 && o[0] == trns_rgb[1] && o[1] == trns_rgb[3] && o[2] == trns_rgb[5] ? 0 : 255;
                        break;
                    case 3: {
                        u32 i;
                        if (bits == 8) i = r[x];
                        else { u32 bit = x * bits; i = (r[bit / 8] >> (8 - bits - bit % 8)) & ((1u << bits) - 1); }
                        memcpy(o, pal[i & 255], 4);
                        break;
                    }
                    case 4: o[0] = o[1] = o[2] = (u8)sample(r, x, bits, 2, 0); o[3] = (u8)sample(r, x, bits, 2, 1); break;
                    default:
                        for (int k = 0; k < 4; k++) o[k] = (u8)sample(r, x, bits, 4, (u32)k);
                }
            }
        }
        free(tmp);
    }
    free(rows);
    free(raw);
    *ow = w;
    *oh = h;
    *has_alpha = ctype == 4 || ctype == 6 || has_trns;
    return rgba;
}
