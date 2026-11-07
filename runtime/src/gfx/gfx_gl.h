/* Render-thread interface of the GL backend (used by platform layers). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "gl.h"

void gfx_gl_init(uint32_t w, uint32_t h);
/* compiled-shader cache in <base>/typex2-shaders/<game> (X2_SHADER_CACHE=0: off) */
void gfx_gl_set_cache_dir(const char *base, const char *game);
/* Replay queued guest frames; waits up to timeout_ms for one. Returns true if
 * a frame was presented. tex, w and h describe the image to show (D3D
 * orientation: row 0 is the top of the picture). */
bool gfx_gl_run_frames(uint32_t timeout_ms, GLuint *tex, uint32_t *w, uint32_t *h);
/* unbind every texture unit (after code outside the replay bound textures) */
void gfx_gl_unbind_textures(void);
/* External frame clock (libretro): instead of pacing itself to 60 Hz, the
 * guest's Present waits for gfx_clock_release(lead), once per front-end frame:
 * the guest may then present up to <frames replayed> + 1 + lead (lead 0 = lockstep). */
void gfx_set_external_clock(bool on);
void gfx_clock_release(uint32_t lead);
/* gfx.c-side statistics for overlays */
uint64_t gfx_frame_count(void);
double gfx_guest_work_ms(void);
void gfx_pace_max(double *guest_ms, double *replay_ms);
