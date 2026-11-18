/* The recorded command stream, shared by the recorder (gfx_record.c, guest
 * threads) and the backends that replay it on the render thread (gfx_gl.c). */
#pragma once
#include <stddef.h>

#include "../core.h"
#include "gfx.h"

enum {
    C_TEX_CREATE = 1,
    C_TEX_UPLOAD,
    C_TEX_DESTROY,
    C_BUF_CREATE,
    C_BUF_UPLOAD,
    C_BUF_DESTROY,
    C_SHADER_CREATE,
    C_SHADER_DESTROY,
    C_DECL_CREATE,
    C_CONSTS,
    C_CLEAR,
    C_DRAW,
    C_DRAW_UP,
    C_PRESENT,
    C_BLIT,
};

typedef struct Frame {
    u8 *data;
    size_t len, cap;
    struct Frame *next;
    u32 refs; /* deferred texture uploads still pointing into data (replay side) */
} Frame;

/* scale: the allocated size over the size the game sees, in 1/256 (GFX_SCALE_ONE = the same) */
typedef struct { GfxId id; u32 fmt, w, h, levels, rt, scale, cube; } TexCreate;
static inline u32 gfx_scaled(u32 v, u32 scale) {
    u32 r = (u32)(((u64)v * scale + GFX_SCALE_ONE / 2) / GFX_SCALE_ONE);
    return r ? r : 1;
}
/* a coordinate (edge) the game gives in its pixels, on a target allocated at scale */
static inline s64 gfx_scale_px(s64 v, u32 scale) {
    return (v * (s64)scale + (s64)GFX_SCALE_ONE / 2) / (s64)GFX_SCALE_ONE;
}
typedef struct { GfxId id; u32 level, x, y, w, h, pitch, bytes, fmt; } TexUpload;
typedef struct { GfxId id; u32 size, index; } BufCreate;
typedef struct { GfxId id; u32 offset, size; } BufUpload;
typedef struct { GfxId id; u32 pixel, ntokens; } ShaderCreate;
typedef struct { GfxId id; u32 count; } DeclCreate;
typedef struct { u32 pixel, start, count; } Consts;
typedef struct { u32 flags, color, stencil; float z; GfxState st; } Clear;
typedef struct {
    GfxId src, dst;
    u32 s[4], d[4], linear;
} Blit;
typedef struct { GfxState st; u32 prim, start, count, indexed; s32 base; u32 min_index, nverts; } Draw;
typedef struct { GfxState st; u32 prim, count, vbytes, stride, ibytes, isz; } DrawUp;

/* bytes per block of a texture format; *bw = block width (4 for DXT, else 1) */
u32 gfx_fmt_block_bytes(u32 f, u32 *bw);

/* ---- replay side's access to the queue (gfx_record.c) ---- */
/* The queued frames, oldest first (linked by next), waiting up to timeout_ms
 * for one; NULL if none. The backend replays every command of every frame:
 * resource commands must never be dropped. */
Frame *gfx_take_frames(u32 timeout_ms);
void gfx_frame_free(Frame *f);
/* called on the recording thread for every texture created, ahead of its replay (the Vulkan
 * backend allocates memory for them meanwhile); NULL when unused */
extern void (*gfx_tex_create_hook)(const TexCreate *k);
/* a frame's C_PRESENT was replayed (the external clock counts them) */
void gfx_frame_replayed(void);
/* how long replaying the frames of one gfx_take_frames took (X2_PACELOG) */
void gfx_replay_time(u64 ns);

/* gfx_texconv.c: DXT1/3/5 block rows -> w x h RGBA8 (malloc'd); 16-bit colour rows -> BGRA8 */
u8 *gfx_dxt_decode(u32 fmt, const u8 *src, u32 w, u32 h);
void gfx_expand16(u32 fmt, const u8 *src, u32 pitch, u32 w, u32 h, u8 *dst);
/* a DXT1 texture without transparent texels (stored as RGB565 on GPUs without S3TC);
 * RGBA8 -> RGB565 in place (R in the high bits) */
bool gfx_dxt1_has_alpha(const u8 *blocks, u32 bytes);
u8 *gfx_rgba_to_565(u8 *rgba, u32 n);
