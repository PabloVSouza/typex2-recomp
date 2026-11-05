/* Font backend for platforms without one yet: no device fonts. */
#include "font.h"

X2Font *x2_font_open(const char *face, int weight, float em) { (void)face; (void)weight; (void)em; return 0; }
void x2_font_close(X2Font *f) { (void)f; }
void x2_font_metrics(X2Font *f, X2FontMetrics *m) { (void)f; *m = (X2FontMetrics){0}; }
bool x2_font_glyph(X2Font *f, uint32_t cp, X2GlyphMetrics *gm, X2PathFn path, void *ud) {
    (void)f; (void)cp; (void)gm; (void)path; (void)ud;
    return false;
}
uint8_t *x2_font_raster(X2Font *f, uint32_t cp, int *w, int *h, int *ox, int *oy, float *advance) {
    (void)f; (void)cp; (void)w; (void)h; (void)ox; (void)oy; (void)advance;
    return 0;
}
