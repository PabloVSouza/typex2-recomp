/* Overlays drawn over the game image by a front end (platform_sdl.c, platform_libretro.c): the
 * FPS counter and the on-screen touch pad. Coordinates are pixels of the target the front end
 * draws into (a window, or the core's output image), top-left origin. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { X2_OV_GL, X2_OV_VULKAN, X2_OV_METAL } X2OvApi;

/* the API the quads go to (GL draws at once into the bound framebuffer; Vulkan and Metal queue
 * them for the backend's next present or output) */
void x2_ov_api(X2OvApi api);
/* the GL context was lost or replaced: textures and programs are made again */
void x2_ov_lost(void);

/* ---- FPS counter: "GAME 60.0  DISP 60.0  CPU 7.2MS" ---- */
/* once per front-end frame; swapped: a frame was shown */
void x2_fps_tick(bool swapped);
void x2_fps_draw(int w, int h);

/* ---- touch pad (player 1): stick, the game's buttons, COIN / START, TEST ---- */
/* lay the pad out for a w x h target; portrait: the bottom 40% under the picture */
void x2_touch_layout(int w, int h, bool portrait);
/* the buttons changed (X2_TOUCH_BUTTONS): labels are made again */
void x2_touch_relabel(void);
enum { X2_FINGER_DOWN, X2_FINGER_MOVE, X2_FINGER_UP };
void x2_touch_finger(uint64_t id, int what, float x, float y);
void x2_touch_release_all(void);
/* what the fingers hold: JVS bits, COIN, TEST */
void x2_touch_state(uint16_t *bits, bool *coin, bool *test);
void x2_touch_draw(int w, int h);
