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

typedef struct { GfxId id; u32 fmt, w, h, levels, rt, scale, cube; } TexCreate;
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
/* a frame's C_PRESENT was replayed (the external clock counts them) */
void gfx_frame_replayed(void);
/* how long replaying the frames of one gfx_take_frames took (X2_PACELOG) */
void gfx_replay_time(u64 ns);
