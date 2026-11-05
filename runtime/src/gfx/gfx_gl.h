/* Render-thread interface of the GL backend (used by platform layers). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "gl.h"

void gfx_gl_init(uint32_t w, uint32_t h);
/* Replay queued guest frames; waits up to timeout_ms for one. Returns true if
 * a frame was presented. tex, w and h describe the image to show (D3D
 * orientation: row 0 is the top of the picture). */
bool gfx_gl_run_frames(uint32_t timeout_ms, GLuint *tex, uint32_t *w, uint32_t *h);
