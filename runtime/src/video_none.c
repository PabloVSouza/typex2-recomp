/* No movie decoder in this build: movies complete immediately. */
#include "video.h"

X2Video *x2_video_open(const char *p) { (void)p; return 0; }
void x2_video_close(X2Video *v) { (void)v; }
void x2_video_info(X2Video *v, int *w, int *h, double *d) { (void)v; *w = 1280; *h = 720; *d = 0; }
void x2_video_run(X2Video *v) { (void)v; }
void x2_video_pause(X2Video *v) { (void)v; }
void x2_video_seek(X2Video *v, double s) { (void)v; (void)s; }
double x2_video_position(X2Video *v) { (void)v; return 0; }
bool x2_video_finished(X2Video *v) { (void)v; return true; }
bool x2_video_frame_yv12(X2Video *v, uint8_t *d, size_t n) { (void)v; (void)d; (void)n; return false; }
void x2_video_set_volume(X2Video *v, float g) { (void)v; (void)g; }
void x2_video_mix(float *out, uint32_t frames, uint32_t rate) { (void)out; (void)frames; (void)rate; }
