/* Backend-neutral rendering interface fed by the D3D9 front end.
 *
 * The front end runs on the guest thread and only records. Everything below
 * appends to the current frame's command list; gfx_present() hands the frame
 * to the render thread (SDL main thread, or a libretro retro_run call), which
 * replays it on the real graphics API. Pointers passed in are copied. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef uint32_t GfxId;  /* 0 = none */

enum GfxFormat {
    GFX_FMT_UNKNOWN = 0,
    GFX_FMT_BGRA8,      /* D3DFMT_A8R8G8B8 */
    GFX_FMT_BGRX8,      /* D3DFMT_X8R8G8B8 */
    GFX_FMT_B5G6R5,
    GFX_FMT_BGR5A1,
    GFX_FMT_BGRA4,
    GFX_FMT_A8,
    GFX_FMT_L8,
    GFX_FMT_A8L8,
    GFX_FMT_DXT1,
    GFX_FMT_DXT3,
    GFX_FMT_DXT5,
    GFX_FMT_R16F,
    GFX_FMT_RG16F,
    GFX_FMT_RGBA16F,
    GFX_FMT_R32F,
    GFX_FMT_RGBA32F,
    GFX_FMT_D24S8,
    GFX_FMT_D16,
};

typedef struct {
    float x, y, w, h, minz, maxz;
} GfxViewport;

/* Snapshot of everything a draw depends on. The front end keeps one current
 * copy and the recorder stores it by value per draw. */
#define GFX_MAX_SAMPLERS 16
#define GFX_MAX_STREAMS 8
#define GFX_VS_CONSTS 256
#define GFX_PS_CONSTS 224
typedef struct {
    uint32_t rs[256];                     /* D3DRENDERSTATETYPE -> value */
    uint32_t samp[GFX_MAX_SAMPLERS][14];  /* D3DSAMPLERSTATETYPE */
    GfxId tex[GFX_MAX_SAMPLERS];
    GfxId vs, ps, decl;
    GfxId vb[GFX_MAX_STREAMS];
    uint32_t vb_offset[GFX_MAX_STREAMS], vb_stride[GFX_MAX_STREAMS];
    GfxId ib;
    uint32_t ib_index_size;
    GfxId rt[4], ds;
    GfxViewport vp;
    int32_t scissor[4];
} GfxState;

void gfx_init(void);
void gfx_texture_create(GfxId id, enum GfxFormat fmt, uint32_t w, uint32_t h, uint32_t levels, bool render_target);
/* render target allocated at scale x (w, h); viewports and scissors on it are scaled to match */
void gfx_texture_create_scaled(GfxId id, enum GfxFormat fmt, uint32_t w, uint32_t h, uint32_t levels, bool render_target,
                               uint32_t scale);
/* cube map; uploads address face f (+X -X +Y -Y +Z -Z), level l as level (f << 4 | l) */
void gfx_texture_create_cube(GfxId id, enum GfxFormat fmt, uint32_t size, uint32_t levels);
void gfx_texture_upload(GfxId id, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void *data,
                        uint32_t pitch);
void gfx_texture_destroy(GfxId id);
void gfx_buffer_create(GfxId id, uint32_t size, bool index);
void gfx_buffer_upload(GfxId id, uint32_t offset, uint32_t size, const void *data);
void gfx_buffer_destroy(GfxId id);
/* D3D9 shader bytecode (vs_3_0 / ps_3_0 tokens) */
void gfx_shader_create(GfxId id, bool pixel, const uint32_t *tokens, uint32_t ntokens);
void gfx_shader_destroy(GfxId id);
/* D3DVERTEXELEMENT9 array (8 bytes each) terminated by D3DDECL_END */
void gfx_decl_create(GfxId id, const void *elements, uint32_t count);
void gfx_set_consts(bool pixel, uint32_t start, const float *v, uint32_t count4);
/* integer constants (count ivec4s) or boolean constants (count ints) */
void gfx_set_consts_ib(bool pixel, bool boolean, uint32_t start, const int32_t *v, uint32_t count);
void gfx_clear(uint32_t flags, uint32_t color, float z, uint32_t stencil, const GfxState *st);
/* prim: D3DPRIMITIVETYPE; count = primitive count */
void gfx_draw(const GfxState *st, uint32_t prim, uint32_t start, uint32_t count, bool indexed, int32_t base_vertex,
              uint32_t min_index, uint32_t num_vertices);
/* inline vertex data (DrawPrimitiveUP / DrawIndexedPrimitiveUP) */
void gfx_draw_up(const GfxState *st, uint32_t prim, uint32_t count, const void *verts, uint32_t vbytes,
                 uint32_t stride, const void *idx, uint32_t ibytes, uint32_t index_size);
/* copy a rect (x0, y0, x1, y1 in the game's pixels) between render targets / the backbuffer */
void gfx_blit(GfxId src, const uint32_t srect[4], GfxId dst, const uint32_t drect[4], bool linear);
void gfx_present(void);
void gfx_set_backbuffer(GfxId id); /* texture that Present() shows */
extern GfxId gfx_backbuffer;       /* read by the backends */
uint64_t gfx_frame_count(void);
void gfx_request_capture(void); /* log every draw of the next replayed frame */
double gfx_guest_work_ms(void); /* mean guest ms per frame since the last call */
void gfx_pace_max(double *guest_ms, double *replay_ms);
