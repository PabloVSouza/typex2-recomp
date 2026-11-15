/* Portable font backend (Android, Linux) on stb_truetype.
 *
 * The games ask for Japanese faces (Fontworks FOT-Rodin family); any of them is
 * served by the first CJK system font stb_truetype can read (Android: the
 * vendor CJK font or Noto Serif CJK; Noto Sans CJK is CFF2, which stb cannot read),
 * with a Latin font as fallback for missing glyphs. X2_FONT=<file.ttf|.ttc|.otf>
 * overrides the search. Bold requests are emboldened (outlines grown like FreeType's
 * FT_Outline_Embolden, bitmaps dilated): system CJK fonts usually ship one weight. */
#define _GNU_SOURCE /* strcasestr */
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#ifndef _WIN32
#include <sys/mman.h>
#endif
#endif
#include <sys/stat.h>
#include <unistd.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "third_party/stb_truetype.h"
#pragma GCC diagnostic pop

#include "core.h"
#include "font.h"

typedef struct {
    stbtt_fontinfo info;
    bool ok;
} Face;

static Face faces[2]; /* [0] primary (CJK), [1] fallback (Latin) */
static pthread_once_t faces_once = PTHREAD_ONCE_INIT;

/* true if the face has a Japanese-language (ja-JP, Windows platform) name record:
 * picks the JP face of a pan-CJK collection (Han glyph shapes differ per region) */
static bool face_is_japanese(const stbtt_fontinfo *fi) {
    int len;
    return stbtt_GetFontNameString(fi, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP,
                                   STBTT_MS_LANG_JAPANESE, 1) != NULL;
}

/* Load a font file. need_cjk: only accept a face that maps Japanese text; in a
 * collection the Japanese face is preferred. stb_truetype reads TrueType and
 * CFF outlines but not CFF2 (variable OpenType, e.g. Android's Noto Sans CJK):
 * such faces fail InitFont and are skipped. */
static bool load_face(Face *f, const char *path, bool need_cjk) {
#ifdef _WIN32
    int fd = open(path, O_RDONLY | O_BINARY);
#else
    int fd = open(path, O_RDONLY);
#endif
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) || st.st_size < 16) { close(fd); return false; }
#ifdef _WIN32 /* no mmap: read it (fonts are a few MB, kept for the session) */
    void *p = malloc((size_t)st.st_size);
    if (!p || read(fd, p, (unsigned)st.st_size) != st.st_size) {
        free(p);
        close(fd);
        return false;
    }
    close(fd);
#else
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return false;
#endif
    int n = stbtt_GetNumberOfFonts(p), best = -1;
    stbtt_fontinfo fi;
    for (int i = 0; i < n; i++) {
        if (!stbtt_InitFont(&fi, p, stbtt_GetFontOffsetForIndex(p, i))) continue;
        if (need_cjk && !(stbtt_FindGlyphIndex(&fi, 0x3042) && stbtt_FindGlyphIndex(&fi, 0x6F22))) continue;
        if (best < 0) best = i;
        if (n == 1 || face_is_japanese(&fi)) { best = i; break; }
    }
    if (best < 0) {
#ifdef _WIN32
        free(p);
#else
        munmap(p, (size_t)st.st_size);
#endif
        return false;
    }
    stbtt_InitFont(&f->info, p, stbtt_GetFontOffsetForIndex(p, best));
    f->ok = true;
    x2_log("font: using %s (face %d)", path, best);
    return true;
}

/* any loadable CJK font in a font directory, sans-serif names first */
static void scan_dir(Face *f, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    char names[256][256];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 256)
        if (strcasestr(e->d_name, "cjk") || strcasestr(e->d_name, "fallback") || strcasestr(e->d_name, "jp") ||
            strcasestr(e->d_name, "gothic"))
            snprintf(names[n++], sizeof names[0], "%s", e->d_name);
    closedir(d);
    for (int pass = 0; pass < 2 && !f->ok; pass++)
        for (int i = 0; i < n && !f->ok; i++) {
            bool sans = !strcasestr(names[i], "serif");
            if (sans != (pass == 0)) continue;
            char path[600];
            snprintf(path, sizeof path, "%s/%s", dir, names[i]);
            load_face(f, path, true);
        }
}

static void load_faces(void) {
    static const char *cjk[] = {
        "/system/fonts/NotoSansCJK-Regular.ttc", "/system/fonts/NotoSansCJKjp-Regular.otf",
        "/system/fonts/SECCJK-Regular.ttc", "/system/fonts/DroidSansFallbackFull.ttf",
        "/system/fonts/DroidSansFallback.ttf", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
    };
    static const char *latin[] = {
        "/system/fonts/Roboto-Regular.ttf", "/system/fonts/RobotoStatic-Regular.ttf", "/system/fonts/DroidSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
    };
    const char *env = getenv("X2_FONT");
    if (!(env && load_face(&faces[0], env, false)))
        for (size_t i = 0; i < sizeof cjk / sizeof cjk[0] && !faces[0].ok; i++) load_face(&faces[0], cjk[i], true);
    if (!faces[0].ok) scan_dir(&faces[0], "/system/fonts");
    if (!faces[0].ok) x2_log("font: no usable CJK font found (set X2_FONT); Japanese text will be missing");
    for (size_t i = 0; i < sizeof latin / sizeof latin[0] && !faces[1].ok; i++) load_face(&faces[1], latin[i], false);
    if (!faces[0].ok && !faces[1].ok) x2_log("font: no system font found (set X2_FONT); text will be missing");
}

struct X2Font {
    float em;
    bool bold, heavy; /* heavy: ultra / extra bold faces */
};

X2Font *x2_font_open(const char *face, int weight, float em) {
    pthread_once(&faces_once, load_faces);
    if (!faces[0].ok && !faces[1].ok) return NULL;
    X2Font *f = calloc(1, sizeof *f);
    f->em = em;
    f->bold = weight >= 700 || (face && (strstr(face, "UB") || strstr(face, "EB") || strstr(face, " B")));
    f->heavy = weight >= 800 || (face && (strstr(face, "UB") || strstr(face, "EB")));
    return f;
}

void x2_font_close(X2Font *f) { free(f); }

static const Face *main_face(void) { return faces[0].ok ? &faces[0] : &faces[1]; }
static float scale_of(const Face *fc, const X2Font *f) { return stbtt_ScaleForMappingEmToPixels(&fc->info, f->em); }

void x2_font_metrics(X2Font *f, X2FontMetrics *m) {
    const Face *fc = main_face();
    /* OS/2 typographic metrics when present: CJK fonts set them to the em box
     * (880/-120), which is what the games lay text out for; hhea values are often
     * much taller (Samsung CJK: 1160/-288) and push glyphs out of text boxes */
    int asc, desc, gap;
    if (!stbtt_GetFontVMetricsOS2(&fc->info, &asc, &desc, &gap)) stbtt_GetFontVMetrics(&fc->info, &asc, &desc, &gap);
    float s = scale_of(fc, f);
    m->ascent = asc * s;
    m->descent = -desc * s;
    m->leading = gap * s;
    m->avg_width = f->em * 0.5f;
    m->max_width = f->em;
}

/* face holding the code point, and its glyph index */
static const Face *find_glyph(uint32_t cp, int *gi) {
    for (int i = 0; i < 2; i++) {
        if (!faces[i].ok) continue;
        int g = stbtt_FindGlyphIndex(&faces[i].info, (int)cp);
        if (g) { *gi = g; return &faces[i]; }
    }
    return NULL;
}

static void emit_cubic(X2PathFn fn, void *ud, float p0x, float p0y, float c1x, float c1y, float c2x, float c2y, float p3x,
                       float p3y) {
    /* split at t = 0.5 and approximate each half with one quadratic */
    float m01x = (p0x + c1x) / 2, m01y = (p0y + c1y) / 2, m12x = (c1x + c2x) / 2, m12y = (c1y + c2y) / 2;
    float m23x = (c2x + p3x) / 2, m23y = (c2y + p3y) / 2;
    float ax = (m01x + m12x) / 2, ay = (m01y + m12y) / 2, bx = (m12x + m23x) / 2, by = (m12y + m23y) / 2;
    float mx = (ax + bx) / 2, my = (ay + by) / 2;
    float q[2][8] = {{p0x, p0y, m01x, m01y, ax, ay, mx, my}, {mx, my, bx, by, m23x, m23y, p3x, p3y}};
    for (int h = 0; h < 2; h++) {
        float *c = q[h];
        float cx = (3 * (c[2] + c[4]) - c[0] - c[6]) / 4, cy = (3 * (c[3] + c[5]) - c[1] - c[7]) / 4;
        fn(ud, 'Q', cx, cy, c[6], c[7]);
    }
}

/* A glyph's outline as path ops ('M', 'L', 'Q'; contours closed implicitly), so it can be
 * emboldened before it is emitted. */
typedef struct {
    char op;
    float x[2], y[2]; /* Q: control, then end; M / L: the point in [0] */
} Op;
typedef struct {
    Op *ops;
    int n, cap;
} Ops;

static void ops_add(void *ud, char op, float x1, float y1, float x2, float y2) {
    Ops *o = ud;
    if (op == 'Z') return;
    if (o->n == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 64;
        Op *grown = realloc(o->ops, (size_t)o->cap * sizeof *o->ops);
        if (!grown) return;
        o->ops = grown;
    }
    o->ops[o->n++] = (Op){op, {x1, x2}, {y1, y2}};
}

/* Synthetic bold on the outline, as FreeType's FT_Outline_Embolden: every point (control points
 * included) moves outward by `r` along the bisector of its two edges' normals, so strokes grow
 * by 2r. System CJK fonts usually ship one weight, and the games ask for heavy ones. */
static void embolden(Ops *o, float r) {
    /* every point of a contour, in order: references into the ops */
    float *px[512], *py[512];
    float area = 0;
    for (int pass = 0; pass < 2; pass++) /* 0: orientation (signed area), 1: move */
        for (int i = 0; i < o->n;) {
            int n = 0, j = i;
            do {
                Op *op = &o->ops[j];
                for (int k = 0; k < (op->op == 'Q' ? 2 : 1) && n < 512; k++, n++) {
                    px[n] = &op->x[k];
                    py[n] = &op->y[k];
                }
                j++;
            } while (j < o->n && o->ops[j].op != 'M');
            i = j;
            bool closed = n > 1 && *px[n - 1] == *px[0] && *py[n - 1] == *py[0]; /* the last point repeats the first */
            if (closed) n--;
            if (n < 3) continue;
            if (pass == 0) {
                for (int k = 0; k < n; k++) area += *px[k] * *py[(k + 1) % n] - *px[(k + 1) % n] * *py[k];
                continue;
            }
            /* outward normal of edge (dx, dy): (-dy, dx) for clockwise outer contours (TrueType,
             * y up), (dy, -dx) for counter-clockwise ones (CFF) */
            float sign = area < 0 ? 1.f : -1.f, dx[512], dy[512];
            for (int k = 0; k < n; k++) {
                int p = (k + n - 1) % n, q = (k + 1) % n;
                float ax = *px[k] - *px[p], ay = *py[k] - *py[p], bx = *px[q] - *px[k], by = *py[q] - *py[k];
                float la = sqrtf(ax * ax + ay * ay), lb = sqrtf(bx * bx + by * by);
                if (la < 1e-6f || lb < 1e-6f) {
                    dx[k] = dy[k] = 0;
                    continue;
                }
                float n1x = -ay / la * sign, n1y = ax / la * sign, n2x = -by / lb * sign, n2y = bx / lb * sign;
                float d = 1 + n1x * n2x + n1y * n2y; /* 1 + cos: the miter grows as the corner sharpens */
                d = r / (d < 0.25f ? 0.25f : d);
                dx[k] = (n1x + n2x) * d;
                dy[k] = (n1y + n2y) * d;
            }
            for (int k = 0; k < n; k++) {
                *px[k] += dx[k];
                *py[k] += dy[k];
            }
            if (closed) { /* the repeated closing point follows the first */
                *px[n] = *px[0];
                *py[n] = *py[0];
            }
        }
}

/* the bold stroke growth for a font: what FreeType uses (em / 24), more for ultra-bold faces */
static float bold_strength(const X2Font *f) { return f->bold ? f->em / (f->heavy ? 14.f : 24.f) : 0.f; }

bool x2_font_glyph(X2Font *f, uint32_t cp, X2GlyphMetrics *gm, X2PathFn path, void *ud) {
    int gi;
    const Face *fc = find_glyph(cp, &gi);
    if (!fc) return false;
    float s = scale_of(fc, f), bold = bold_strength(f);
    int adv, lsb, x0, y0, x1, y1;
    stbtt_GetGlyphHMetrics(&fc->info, gi, &adv, &lsb);
    if (!stbtt_GetGlyphBox(&fc->info, gi, &x0, &y0, &x1, &y1)) x0 = y0 = x1 = y1 = 0;
    bool empty = x1 <= x0 || y1 <= y0;
    /* emboldened: wider by `bold`, the glyph moved right by half of it so its left bearing stays */
    gm->advance = adv * s + bold;
    gm->bbox_x = x0 * s;
    gm->bbox_y = y0 * s - (empty ? 0 : bold / 2);
    gm->bbox_w = empty ? 0 : (x1 - x0) * s + bold;
    gm->bbox_h = empty ? 0 : (y1 - y0) * s + bold;
    if (path) {
        stbtt_vertex *v;
        int n = stbtt_GetGlyphShape(&fc->info, gi, &v);
        Ops o = {0};
        float cx = 0, cy = 0;
        for (int i = 0; i < n; i++) {
            float x = v[i].x * s, y = v[i].y * s;
            switch (v[i].type) {
            case STBTT_vmove: ops_add(&o, 'M', x, y, 0, 0); break;
            case STBTT_vline: ops_add(&o, 'L', x, y, 0, 0); break;
            case STBTT_vcurve: ops_add(&o, 'Q', v[i].cx * s, v[i].cy * s, x, y); break;
            case STBTT_vcubic:
                emit_cubic(ops_add, &o, cx, cy, v[i].cx * s, v[i].cy * s, v[i].cx1 * s, v[i].cy1 * s, x, y);
                break;
            }
            cx = x;
            cy = y;
        }
        stbtt_FreeShape(&fc->info, v);
        if (bold > 0) {
            embolden(&o, bold / 2);
            for (int i = 0; i < o.n; i++) /* keep the left side bearing */
                for (int k = 0; k < 2; k++) o.ops[i].x[k] += bold / 2;
        }
        for (int i = 0; i < o.n; i++) {
            Op *op = &o.ops[i];
            if (op->op == 'M' && i > 0) path(ud, 'Z', 0, 0, 0, 0);
            path(ud, op->op, op->x[0], op->y[0], op->x[1], op->y[1]);
        }
        if (o.n) path(ud, 'Z', 0, 0, 0, 0);
        free(o.ops);
    }
    return true;
}

uint8_t *x2_font_raster(X2Font *f, uint32_t cp, int *w, int *h, int *ox, int *oy, float *advance) {
    int gi;
    const Face *fc = find_glyph(cp, &gi);
    if (!fc) return NULL;
    float s = scale_of(fc, f);
    int adv, lsb, ix0, iy0, ix1, iy1;
    stbtt_GetGlyphHMetrics(&fc->info, gi, &adv, &lsb);
    stbtt_GetGlyphBitmapBox(&fc->info, gi, s, s, &ix0, &iy0, &ix1, &iy1); /* y down */
    int gw = ix1 - ix0 > 0 ? ix1 - ix0 : 1, gh = iy1 - iy0 > 0 ? iy1 - iy0 : 1;
    int grow = f->bold ? (int)fmaxf(1.f, lrintf(bold_strength(f))) : 0; /* synthetic bold, in pixels */
    int bw = gw + grow;
    uint8_t *px = calloc((size_t)bw * gh, 1);
    if (!px) return NULL;
    if (ix1 > ix0 && iy1 > iy0) stbtt_MakeGlyphBitmap(&fc->info, px, gw, gh, bw, s, s, gi);
    for (int g = 0; g < grow; g++) /* widen strokes one pixel per pass */
        for (int y = 0; y < gh; y++)
            for (int x = bw - 1; x > 0; x--) {
                uint8_t *p = px + (size_t)y * bw + x;
                if (p[-1] > p[0]) p[0] = p[-1];
            }
    *w = bw;
    *h = gh;
    *ox = ix0;
    *oy = -iy0; /* top edge in pen space, y up */
    *advance = adv * s + (float)grow;
    return px;
}
