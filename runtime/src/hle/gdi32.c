/* gdi32: the font calls Scaleform's system-font provider makes, backed by the
 * host font backend (font.h). GetGlyphOutlineW produces GDI's GGO_NATIVE
 * TrueType outline records (TTPOLYGONHEADER + TTPOLYCURVE). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../core.h"
#include "../font.h"

#define GDI_ERROR 0xFFFFFFFFu
#define GGO_METRICS 0
#define GGO_NATIVE 2
#define TT_POLYGON_TYPE 24
#define TT_PRIM_LINE 1
#define TT_PRIM_QSPLINE 2

typedef struct {
    X2Font *font;
    s32 height, weight;
    char face[128];
} Font;

#define MAX_FONTS 256
#define FONT_BASE 0x00030000u
static Font fonts[MAX_FONTS];
static u32 selected; /* currently selected font handle (one shared DC) */
static pthread_mutex_t gdi_lock = PTHREAD_MUTEX_INITIALIZER;

static Font *font_of(u32 h) {
    if (h < FONT_BASE + 4 || (h - FONT_BASE) % 4) return NULL;
    u32 i = (h - FONT_BASE) / 4 - 1;
    return i < MAX_FONTS && fonts[i].font ? &fonts[i] : NULL;
}

static void utf16_to_utf8(u32 w, char *out, size_t n) {
    size_t k = 0;
    for (; w && RD16(w) && k + 4 < n; w += 2) {
        u32 ch = RD16(w);
        if (ch < 0x80) out[k++] = (char)ch;
        else if (ch < 0x800) { out[k++] = (char)(0xC0 | ch >> 6); out[k++] = (char)(0x80 | (ch & 0x3F)); }
        else { out[k++] = (char)(0xE0 | ch >> 12); out[k++] = (char)(0x80 | (ch >> 6 & 0x3F)); out[k++] = (char)(0x80 | (ch & 0x3F)); }
    }
    out[k] = 0;
}

static void EnumFontFamiliesExW(X2Cpu *c) {
    u32 lf = ARG(1), proc = ARG(2), lparam = ARG(3);
    char face[128];
    utf16_to_utf8(lf ? lf + 28 : 0, face, sizeof face);
    /* every face is "available": the backend maps it onto a system font */
    u32 elf = x2_galloc(348 + 128), ntm = x2_galloc(128);
    memcpy(X2P(elf), X2P(lf), 92);
    for (u32 i = 0; i < 32 && RD16(lf + 28 + 2 * i); i++) WR16(elf + 92 + 2 * i, RD16(lf + 28 + 2 * i));
    WR32(ntm, 16);                 /* tmHeight */
    WR32(ntm + 28, 400);           /* tmWeight */
    WR8(ntm + 55, 0x06);           /* tmPitchAndFamily: TMPF_VECTOR | TMPF_TRUETYPE */
    WR8(ntm + 56, 128);            /* tmCharSet: SHIFTJIS */
    X2_TRACE("EnumFontFamiliesExW(\"%s\") -> one TrueType face", face);
    u32 r = x2_guest_call(c, proc, 4, elf, ntm, 4 /* TRUETYPE_FONTTYPE */, lparam);
    x2_heap_free(0, elf);
    x2_heap_free(0, ntm);
    RET(r);
}

/* CreateFontW(height, width, escapement, orientation, weight, italic, underline,
 *             strikeout, charset, outprec, clipprec, quality, pitch, face) */
static void CreateFontW(X2Cpu *c) {
    s32 height = (s32)ARG(0);
    s32 weight = (s32)ARG(4);
    char face[128];
    utf16_to_utf8(ARG(13), face, sizeof face);
    float em = height < 0 ? (float)-height : height > 0 ? (float)height : 16.0f;
    X2Font *f = x2_font_open(face, weight ? weight : 400, em);
    if (f && height > 0) { /* positive height = cell height: rescale to that */
        X2FontMetrics m;
        x2_font_metrics(f, &m);
        float cell = m.ascent + m.descent;
        if (cell > 0) {
            x2_font_close(f);
            f = x2_font_open(face, weight ? weight : 400, em * em / cell);
        }
    }
    if (!f) {
        x2_log("CreateFontW(\"%s\"): no host font", face);
        RET(0);
        return;
    }
    pthread_mutex_lock(&gdi_lock);
    u32 h = 0;
    for (u32 i = 0; i < MAX_FONTS; i++)
        if (!fonts[i].font) {
            fonts[i] = (Font){f, height, weight, ""};
            snprintf(fonts[i].face, sizeof fonts[i].face, "%s", face);
            h = FONT_BASE + 4 * (i + 1);
            break;
        }
    pthread_mutex_unlock(&gdi_lock);
    if (!h) x2_font_close(f);
    X2_TRACE("CreateFontW(height %d, weight %d, \"%s\") -> %08x", height, weight, face, h);
    RET(h);
}

static void SelectObject(X2Cpu *c) {
    u32 obj = ARG(1), prev = selected;
    if (font_of(obj)) selected = obj;
    RET(prev ? prev : FONT_BASE);
}
static void DeleteObject(X2Cpu *c) {
    pthread_mutex_lock(&gdi_lock);
    Font *f = font_of(ARG(0));
    if (f) {
        x2_font_close(f->font);
        f->font = NULL;
        if (selected == ARG(0)) selected = 0;
    }
    pthread_mutex_unlock(&gdi_lock);
    RET(1);
}

static void GetTextMetricsW(X2Cpu *c) {
    u32 tm = ARG(1);
    memset(X2P(tm), 0, 57);
    Font *f = font_of(selected);
    if (!f) { RET(0); return; }
    X2FontMetrics m;
    x2_font_metrics(f->font, &m);
    s32 asc = (s32)ceilf(m.ascent), desc = (s32)ceilf(m.descent);
    WR32(tm, (u32)(asc + desc));             /* tmHeight */
    WR32(tm + 4, (u32)asc);
    WR32(tm + 8, (u32)desc);
    WR32(tm + 12, 0);                        /* tmInternalLeading */
    WR32(tm + 16, (u32)(s32)ceilf(m.leading));
    WR32(tm + 20, (u32)(s32)ceilf(m.avg_width));
    WR32(tm + 24, (u32)(s32)ceilf(m.max_width));
    WR32(tm + 28, (u32)(f->weight ? f->weight : 400));
    WR32(tm + 36, 96);                       /* tmDigitizedAspectX/Y */
    WR32(tm + 40, 96);
    WR16(tm + 44, 0x20);                     /* tmFirstChar */
    WR16(tm + 46, 0xFFFC);                   /* tmLastChar */
    WR16(tm + 48, 0x30FB);                   /* tmDefaultChar */
    WR16(tm + 50, 0x20);                     /* tmBreakChar */
    WR8(tm + 55, 0x06);                      /* TMPF_VECTOR | TMPF_TRUETYPE */
    WR8(tm + 56, 128);                       /* SHIFTJIS_CHARSET */
    RET(1);
}

static void GetKerningPairsW(X2Cpu *c) { RET(0); }

/* ---- GGO_NATIVE outline builder ------------------------------------------- */
typedef struct {
    u8 *buf;
    size_t len, cap;
    size_t header; /* offset of the current TTPOLYGONHEADER, or SIZE_MAX */
} Outline;

static void put(Outline *o, const void *p, size_t n) {
    if (o->len + n > o->cap) {
        o->cap = (o->cap + n) * 2;
        o->buf = realloc(o->buf, o->cap);
    }
    memcpy(o->buf + o->len, p, n);
    o->len += n;
}
static void put_fixed(Outline *o, float v) {
    s32 f = (s32)lrintf(v * 65536.0f);
    put(o, &f, 4);
}
static void close_contour(Outline *o) {
    if (o->header == (size_t)-1) return;
    u32 cb = (u32)(o->len - o->header);
    memcpy(o->buf + o->header, &cb, 4);
    o->header = (size_t)-1;
}
static void path_cb(void *ud, char op, float x1, float y1, float x2, float y2) {
    Outline *o = ud;
    u16 type, n;
    switch (op) {
        case 'M': {
            close_contour(o);
            o->header = o->len;
            u32 zero = 0, t = TT_POLYGON_TYPE;
            put(o, &zero, 4); /* cb, patched on close */
            put(o, &t, 4);
            put_fixed(o, x1);
            put_fixed(o, y1);
            break;
        }
        case 'L':
            type = TT_PRIM_LINE; n = 1;
            put(o, &type, 2); put(o, &n, 2);
            put_fixed(o, x1); put_fixed(o, y1);
            break;
        case 'Q':
            type = TT_PRIM_QSPLINE; n = 2;
            put(o, &type, 2); put(o, &n, 2);
            put_fixed(o, x1); put_fixed(o, y1);
            put_fixed(o, x2); put_fixed(o, y2);
            break;
        case 'Z':
            close_contour(o);
            break;
    }
}

/* GetGlyphOutlineW(hdc, ch, format, GLYPHMETRICS*, cbBuffer, pvBuffer, MAT2*) */
static void GetGlyphOutlineW(X2Cpu *c) {
    u32 ch = ARG(1), fmt = ARG(2) & 0xFF, gmp = ARG(3), cb = ARG(4), buf = ARG(5);
    Font *f = font_of(selected);
    /* GGO_BITMAP (1) and GGO_GRAY2/4/8 (4/5/6): rasterized glyphs */
    if (f && (fmt == 1 || (fmt >= 4 && fmt <= 6))) {
        int w, h, ox, oy;
        float adv;
        pthread_mutex_lock(&gdi_lock);
        u8 *px = x2_font_raster(f->font, ch, &w, &h, &ox, &oy, &adv);
        pthread_mutex_unlock(&gdi_lock);
        if (!px) { RET(GDI_ERROR); return; }
        if (gmp) {
            WR32(gmp, (u32)w); WR32(gmp + 4, (u32)h);
            WR32(gmp + 8, (u32)ox); WR32(gmp + 12, (u32)oy);
            WR16(gmp + 16, (u16)(s16)lrintf(adv)); WR16(gmp + 18, 0);
        }
        u32 levels = fmt == 4 ? 4 : fmt == 5 ? 16 : 64;
        u32 pitch = fmt == 1 ? ((u32)w + 31) / 32 * 4 : ((u32)w + 3) & ~3u;
        u32 need = pitch * (u32)h;
        if (!buf || !cb) { free(px); RET(need); return; }
        if (cb < need) { free(px); RET(GDI_ERROR); return; }
        memset(X2P(buf), 0, need);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                u8 v = px[y * w + x];
                if (fmt == 1) {
                    if (v >= 128) WR8(buf + (u32)y * pitch + (u32)x / 8, (u8)(RD8(buf + (u32)y * pitch + (u32)x / 8) | (0x80 >> (x % 8))));
                } else {
                    WR8(buf + (u32)y * pitch + (u32)x, (u8)((v * levels + 127) / 255));
                }
            }
        free(px);
        RET(need);
        return;
    }
    if (!f || (fmt != GGO_NATIVE && fmt != GGO_METRICS)) {
        static int warned;
        if (!warned++) x2_log("GetGlyphOutlineW: format %u or font unsupported", ARG(2));
        RET(GDI_ERROR);
        return;
    }
    Outline o = {NULL, 0, 0, (size_t)-1};
    X2GlyphMetrics gm;
    pthread_mutex_lock(&gdi_lock);
    bool ok = x2_font_glyph(f->font, ch, &gm, fmt == GGO_NATIVE ? path_cb : NULL, &o);
    pthread_mutex_unlock(&gdi_lock);
    close_contour(&o);
    if (!ok) { free(o.buf); RET(GDI_ERROR); return; }
    if (gmp) {
        WR32(gmp, (u32)(s32)ceilf(gm.bbox_w > 0 ? gm.bbox_w : 1));
        WR32(gmp + 4, (u32)(s32)ceilf(gm.bbox_h > 0 ? gm.bbox_h : 1));
        WR32(gmp + 8, (u32)(s32)floorf(gm.bbox_x));
        WR32(gmp + 12, (u32)(s32)ceilf(gm.bbox_y + gm.bbox_h));
        WR16(gmp + 16, (u16)(s16)lrintf(gm.advance));
        WR16(gmp + 18, 0);
    }
    if (fmt == GGO_METRICS) { free(o.buf); RET(1); return; }
    u32 need = (u32)o.len;
    if (!buf || !cb) { free(o.buf); RET(need); return; }
    if (cb < need) { free(o.buf); RET(GDI_ERROR); return; }
    memcpy(X2P(buf), o.buf, need);
    free(o.buf);
    RET(need);
}

/* the screen is a 1920x1080, 32-bit, 96 dpi, 60 Hz display */
static void GetDeviceCaps(X2Cpu *c) {
    switch (ARG(1)) {
        case 8: RET(1920); break;   /* HORZRES */
        case 10: RET(1080); break;  /* VERTRES */
        case 12: RET(32); break;    /* BITSPIXEL */
        case 14: RET(1); break;     /* PLANES */
        case 88: case 90: RET(96); break; /* LOGPIXELSX/Y */
        case 116: RET(60); break;   /* VREFRESH */
        case 117: RET(1920); break; /* DESKTOPHORZRES */
        case 118: RET(1080); break; /* DESKTOPVERTRES */
        default: RET(0); break;
    }
}
static void GetStockObject(X2Cpu *c) { RET(0x00040000u + ARG(0)); }

const X2Hle x2_hle_gdi32[] = {
    {"GetDeviceCaps", GetDeviceCaps, 2},
    {"GetStockObject", GetStockObject, 1},
    {"EnumFontFamiliesExW", EnumFontFamiliesExW, 5},
    {"CreateFontW", CreateFontW, 14},
    {"SelectObject", SelectObject, 2},
    {"DeleteObject", DeleteObject, 1},
    {"GetTextMetricsW", GetTextMetricsW, 2},
    {"GetKerningPairsW", GetKerningPairsW, 3},
    {"GetGlyphOutlineW", GetGlyphOutlineW, 7},
    {NULL, NULL, 0},
};
