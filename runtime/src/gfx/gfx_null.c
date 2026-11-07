/* Null backend: accepts everything, renders nothing. Used until the GL
 * backend lands, and for headless runs (logic tests, CI). */
#include "gfx.h"

static uint64_t frames;

void gfx_init(void) {}
void gfx_texture_create(GfxId id, enum GfxFormat fmt, uint32_t w, uint32_t h, uint32_t levels, bool rt) {}
void gfx_texture_create_scaled(GfxId id, enum GfxFormat fmt, uint32_t w, uint32_t h, uint32_t levels, bool rt, uint32_t s) {}
void gfx_texture_create_cube(GfxId id, enum GfxFormat fmt, uint32_t size, uint32_t levels) {}
void gfx_texture_upload(GfxId id, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void *data,
                        uint32_t pitch) {}
void gfx_texture_destroy(GfxId id) {}
void gfx_buffer_create(GfxId id, uint32_t size, bool index) {}
void gfx_buffer_upload(GfxId id, uint32_t offset, uint32_t size, const void *data) {}
void gfx_buffer_destroy(GfxId id) {}
void gfx_shader_create(GfxId id, bool pixel, const uint32_t *tokens, uint32_t ntokens) {}
void gfx_shader_destroy(GfxId id) {}
void gfx_decl_create(GfxId id, const void *elements, uint32_t count) {}
void gfx_set_consts(bool pixel, uint32_t start, const float *v, uint32_t count4) {}
void gfx_set_consts_ib(bool pixel, bool boolean, uint32_t start, const int32_t *v, uint32_t count) {}
void gfx_clear(uint32_t flags, uint32_t color, float z, uint32_t stencil, const GfxState *st) {}
void gfx_draw(const GfxState *st, uint32_t prim, uint32_t start, uint32_t count, bool indexed, int32_t base_vertex,
              uint32_t min_index, uint32_t num_vertices) {}
void gfx_draw_up(const GfxState *st, uint32_t prim, uint32_t count, const void *verts, uint32_t vbytes,
                 uint32_t stride, const void *idx, uint32_t ibytes, uint32_t index_size) {}
void gfx_present(void) { frames++; }
uint64_t gfx_frame_count(void) { return frames; }
void gfx_gl_set_backbuffer(GfxId id) { (void)id; }
