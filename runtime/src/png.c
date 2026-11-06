/* Minimal PNG writer (screenshots, texture dumps). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "core.h"

static void put32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static void png_chunk(FILE *f, const char *type, const u8 *data, u32 n) {
    u8 hdr[8];
    put32(hdr, n);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uLong crc = crc32(0, (const Bytef *)type, 4);
    if (n) crc = crc32(crc, data, n);
    u8 c[4];
    put32(c, (u32)crc);
    fwrite(c, 1, 4, f);
}
bool x2_write_png(const char *path, const u8 *rgba, u32 w, u32 h) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    u8 ihdr[13];
    put32(ihdr, w);
    put32(ihdr + 4, h);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, 13);
    size_t raw_n = (size_t)(w * 4 + 1) * h;
    u8 *raw = malloc(raw_n);
    for (u32 y = 0; y < h; y++) {
        raw[y * (w * 4 + 1)] = 0;
        memcpy(raw + y * (w * 4 + 1) + 1, rgba + (size_t)y * w * 4, w * 4);
    }
    uLongf zn = compressBound(raw_n);
    u8 *z = malloc(zn);
    compress2(z, &zn, raw, raw_n, 6);
    png_chunk(f, "IDAT", z, (u32)zn);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    return true;
}
