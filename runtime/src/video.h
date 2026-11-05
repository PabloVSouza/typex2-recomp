/* Movie playback backing the DirectShow emulation (FFmpeg when available). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct X2Video X2Video;

X2Video *x2_video_open(const char *host_path); /* NULL if unsupported/unavailable */
void x2_video_close(X2Video *v);
void x2_video_info(X2Video *v, int *w, int *h, double *duration);
void x2_video_run(X2Video *v);    /* start or resume playback (wall-clock paced) */
void x2_video_pause(X2Video *v);
void x2_video_seek(X2Video *v, double seconds);
double x2_video_position(X2Video *v);
bool x2_video_finished(X2Video *v);   /* all streams played out */
/* Copy the current frame as YV12 (Y, V, U planes) at w x h; false if none yet. */
bool x2_video_frame_yv12(X2Video *v, uint8_t *dst, size_t n);
void x2_video_set_volume(X2Video *v, float gain);
/* Mixed into the audio output by the DirectSound mixer. */
void x2_video_mix(float *out, uint32_t frames, uint32_t rate);
