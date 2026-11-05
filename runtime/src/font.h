/* Host font backend behind gdi32's font calls (CoreText on Apple; others TBD). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct X2Font X2Font;

typedef struct {
    float ascent, descent, leading, avg_width, max_width;
} X2FontMetrics;

typedef struct {
    float advance;
    float bbox_x, bbox_y, bbox_w, bbox_h; /* y up, relative to the pen origin */
} X2GlyphMetrics;

/* Path callback: op 'M' move, 'L' line, 'Q' quad (x1,y1 control, x2,y2 end),
 * 'Z' close. Coordinates in pixels, y up from the baseline. */
typedef void (*X2PathFn)(void *ud, char op, float x1, float y1, float x2, float y2);

/* face: requested (UTF-8) family name; weight: 100..900; em: pixel size */
X2Font *x2_font_open(const char *face, int weight, float em);
void x2_font_close(X2Font *f);
void x2_font_metrics(X2Font *f, X2FontMetrics *m);
/* false if the font has no glyph for the code point */
bool x2_font_glyph(X2Font *f, uint32_t codepoint, X2GlyphMetrics *gm, X2PathFn path, void *ud);

/* Rasterize a glyph into an 8-bit coverage bitmap of w x h (top row first),
 * positioned like GDI: the bitmap's top-left is at (ox, oy) in pen space
 * (y up). Returns malloc'd pixels or NULL. */
uint8_t *x2_font_raster(X2Font *f, uint32_t codepoint, int *w, int *h, int *ox, int *oy, float *advance);
