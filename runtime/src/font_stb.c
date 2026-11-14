/* Portable font backend (Android, Linux) on stb_truetype.
 *
 * The games ask for Japanese faces (Fontworks FOT-Rodin family); any of them is
 * served by the first CJK system font stb_truetype can read (Android: the
 * vendor CJK font or Noto Serif CJK; Noto Sans CJK is CFF2, which stb cannot read),
 * with a Latin font as fallback for missing glyphs. X2_FONT=<file.ttf|.ttc|.otf>
 * overrides the search. Bold requests are emboldened by dilating the coverage
 * bitmap (system CJK fonts usually ship a single weight). */
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
    bool bold;
};

X2Font *x2_font_open(const char *face, int weight, float em) {
    pthread_once(&faces_once, load_faces);
    if (!faces[0].ok && !faces[1].ok) return NULL;
    X2Font *f = calloc(1, sizeof *f);
    f->em = em;
    f->bold = weight >= 700 || (face && (strstr(face, "UB") || strstr(face, "EB") || strstr(face, " B")));
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

bool x2_font_glyph(X2Font *f, uint32_t cp, X2GlyphMetrics *gm, X2PathFn path, void *ud) {
    int gi;
    const Face *fc = find_glyph(cp, &gi);
    if (!fc) return false;
    float s = scale_of(fc, f);
    int adv, lsb, x0, y0, x1, y1;
    stbtt_GetGlyphHMetrics(&fc->info, gi, &adv, &lsb);
    if (!stbtt_GetGlyphBox(&fc->info, gi, &x0, &y0, &x1, &y1)) x0 = y0 = x1 = y1 = 0;
    gm->advance = adv * s;
    gm->bbox_x = x0 * s;
    gm->bbox_y = y0 * s;
    gm->bbox_w = (x1 - x0) * s;
    gm->bbox_h = (y1 - y0) * s;
    if (path) {
        stbtt_vertex *v;
        int n = stbtt_GetGlyphShape(&fc->info, gi, &v);
        float cx = 0, cy = 0;
        bool open = false;
        for (int i = 0; i < n; i++) {
            float x = v[i].x * s, y = v[i].y * s;
            switch (v[i].type) {
                case STBTT_vmove:
                    if (open) path(ud, 'Z', 0, 0, 0, 0);
                    path(ud, 'M', x, y, 0, 0);
                    open = true;
                    break;
                case STBTT_vline: path(ud, 'L', x, y, 0, 0); break;
                case STBTT_vcurve: path(ud, 'Q', v[i].cx * s, v[i].cy * s, x, y); break;
                case STBTT_vcubic:
                    emit_cubic(path, ud, cx, cy, v[i].cx * s, v[i].cy * s, v[i].cx1 * s, v[i].cy1 * s, x, y);
                    break;
            }
            cx = x;
            cy = y;
        }
        if (open) path(ud, 'Z', 0, 0, 0, 0);
        stbtt_FreeShape(&fc->info, v);
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
    int bw = gw + (f->bold ? 1 : 0);
    uint8_t *px = calloc((size_t)bw * gh, 1);
    if (!px) return NULL;
    if (ix1 > ix0 && iy1 > iy0) stbtt_MakeGlyphBitmap(&fc->info, px, gw, gh, bw, s, s, gi);
    if (f->bold) /* synthetic bold: widen strokes by one pixel */
        for (int y = 0; y < gh; y++)
            for (int x = bw - 1; x > 0; x--) {
                uint8_t *p = px + (size_t)y * bw + x;
                if (p[-1] > p[0]) p[0] = p[-1];
            }
    *w = bw;
    *h = gh;
    *ox = ix0;
    *oy = -iy0; /* top edge in pen space, y up */
    *advance = adv * s + (f->bold ? 1.f : 0.f);
    return px;
}
