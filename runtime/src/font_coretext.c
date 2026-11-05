/* CoreText font backend. Maps the Japanese faces the games request (Fontworks
 * FOT-Rodin family) onto the system's Hiragino Sans weights. Cubic outlines
 * are split into quadratics so they fit GDI's TrueType outline records. */
#include <CoreText/CoreText.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "font.h"

struct X2Font {
    CTFontRef ct;
};

static const char *pick_postscript_name(const char *face, int weight) {
    /* heavy faces: "UB" (ultra bold), "EB", "B"; light: "L", "M" */
    bool ub = face && (strstr(face, "UB") || strstr(face, "EB"));
    if (ub || weight >= 800) return "HiraginoSans-W8";
    if (weight >= 700 || (face && strstr(face, " B"))) return "HiraginoSans-W6";
    if (face && strstr(face, " M")) return "HiraginoSans-W4";
    return "HiraginoSans-W4";
}

X2Font *x2_font_open(const char *face, int weight, float em) {
    const char *ps = pick_postscript_name(face, weight);
    CFStringRef name = CFStringCreateWithCString(NULL, ps, kCFStringEncodingUTF8);
    CTFontRef ct = CTFontCreateWithName(name, em, NULL);
    CFRelease(name);
    if (!ct) return NULL;
    X2Font *f = calloc(1, sizeof *f);
    f->ct = ct;
    return f;
}

void x2_font_close(X2Font *f) {
    if (!f) return;
    CFRelease(f->ct);
    free(f);
}

void x2_font_metrics(X2Font *f, X2FontMetrics *m) {
    m->ascent = (float)CTFontGetAscent(f->ct);
    m->descent = (float)CTFontGetDescent(f->ct);
    m->leading = (float)CTFontGetLeading(f->ct);
    m->avg_width = (float)CTFontGetSize(f->ct) * 0.5f;
    m->max_width = (float)CTFontGetSize(f->ct);
}

typedef struct {
    X2PathFn fn;
    void *ud;
    CGPoint cur, start;
} Walk;

static void emit_cubic(Walk *w, CGPoint p0, CGPoint c1, CGPoint c2, CGPoint p3) {
    /* split at t = 0.5 and approximate each half with one quadratic */
    CGPoint m01 = {(p0.x + c1.x) / 2, (p0.y + c1.y) / 2}, m12 = {(c1.x + c2.x) / 2, (c1.y + c2.y) / 2};
    CGPoint m23 = {(c2.x + p3.x) / 2, (c2.y + p3.y) / 2};
    CGPoint a = {(m01.x + m12.x) / 2, (m01.y + m12.y) / 2}, b = {(m12.x + m23.x) / 2, (m12.y + m23.y) / 2};
    CGPoint mid = {(a.x + b.x) / 2, (a.y + b.y) / 2};
    CGPoint halves[2][4] = {{p0, m01, a, mid}, {mid, b, m23, p3}};
    for (int h = 0; h < 2; h++) {
        CGPoint *q = halves[h];
        CGPoint ctl = {(3 * (q[1].x + q[2].x) - q[0].x - q[3].x) / 4, (3 * (q[1].y + q[2].y) - q[0].y - q[3].y) / 4};
        w->fn(w->ud, 'Q', (float)ctl.x, (float)ctl.y, (float)q[3].x, (float)q[3].y);
    }
}

static void walk(void *info, const CGPathElement *e) {
    Walk *w = info;
    switch (e->type) {
        case kCGPathElementMoveToPoint:
            w->cur = w->start = e->points[0];
            w->fn(w->ud, 'M', (float)e->points[0].x, (float)e->points[0].y, 0, 0);
            break;
        case kCGPathElementAddLineToPoint:
            w->fn(w->ud, 'L', (float)e->points[0].x, (float)e->points[0].y, 0, 0);
            w->cur = e->points[0];
            break;
        case kCGPathElementAddQuadCurveToPoint:
            w->fn(w->ud, 'Q', (float)e->points[0].x, (float)e->points[0].y, (float)e->points[1].x, (float)e->points[1].y);
            w->cur = e->points[1];
            break;
        case kCGPathElementAddCurveToPoint:
            emit_cubic(w, w->cur, e->points[0], e->points[1], e->points[2]);
            w->cur = e->points[2];
            break;
        case kCGPathElementCloseSubpath:
            w->fn(w->ud, 'Z', 0, 0, 0, 0);
            w->cur = w->start;
            break;
    }
}

bool x2_font_glyph(X2Font *f, uint32_t cp, X2GlyphMetrics *gm, X2PathFn path, void *ud) {
    UniChar chars[2];
    CFIndex n = 1;
    if (cp >= 0x10000) {
        cp -= 0x10000;
        chars[0] = (UniChar)(0xD800 + (cp >> 10));
        chars[1] = (UniChar)(0xDC00 + (cp & 0x3FF));
        n = 2;
    } else {
        chars[0] = (UniChar)cp;
    }
    CGGlyph g[2] = {0, 0};
    if (!CTFontGetGlyphsForCharacters(f->ct, chars, g, n) || !g[0]) return false;
    CGSize adv;
    CTFontGetAdvancesForGlyphs(f->ct, kCTFontOrientationHorizontal, g, &adv, 1);
    CGRect r = CTFontGetBoundingRectsForGlyphs(f->ct, kCTFontOrientationHorizontal, g, NULL, 1);
    gm->advance = (float)adv.width;
    gm->bbox_x = (float)r.origin.x;
    gm->bbox_y = (float)r.origin.y;
    gm->bbox_w = (float)r.size.width;
    gm->bbox_h = (float)r.size.height;
    if (path) {
        CGPathRef p = CTFontCreatePathForGlyph(f->ct, g[0], NULL);
        if (p) {
            Walk w = {path, ud, {0, 0}, {0, 0}};
            CGPathApply(p, &w, walk);
            CGPathRelease(p);
        }
    }
    return true;
}

uint8_t *x2_font_raster(X2Font *f, uint32_t cp, int *w, int *h, int *ox, int *oy, float *advance) {
    X2GlyphMetrics gm;
    if (!x2_font_glyph(f, cp, &gm, NULL, NULL)) return NULL;
    int x0 = (int)floorf(gm.bbox_x), y1 = (int)ceilf(gm.bbox_y + gm.bbox_h);
    int x1 = (int)ceilf(gm.bbox_x + gm.bbox_w), y0 = (int)floorf(gm.bbox_y);
    int bw = x1 - x0 > 0 ? x1 - x0 : 1, bh = y1 - y0 > 0 ? y1 - y0 : 1;
    uint8_t *px = calloc((size_t)bw * bh, 1);
    CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
    CGContextRef ctx = CGBitmapContextCreate(px, (size_t)bw, (size_t)bh, 8, (size_t)bw, gray, kCGImageAlphaNone);
    CGColorSpaceRelease(gray);
    if (!ctx) { free(px); return NULL; }
    CGContextSetGrayFillColor(ctx, 1.0, 1.0);
    UniChar ch = (UniChar)(cp < 0x10000 ? cp : '?');
    CGGlyph g = 0;
    CTFontGetGlyphsForCharacters(f->ct, &ch, &g, 1);
    CGPoint pos = {(CGFloat)-x0, (CGFloat)-y0};  /* CG origin is bottom-left: baseline at -y0 */
    CTFontDrawGlyphs(f->ct, &g, &pos, 1, ctx);
    CGContextRelease(ctx);
    /* CG bitmap memory is top row first already (row 0 = top of the context) */
    *w = bw; *h = bh; *ox = x0; *oy = y1; *advance = gm.advance;
    return px;
}
