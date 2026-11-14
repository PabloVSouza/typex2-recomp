/* The GL entry points of gl_win.h, looked up once a GL context exists (Windows). */
#ifdef _WIN32
#define X2_GL_LOADER
#include "gl.h"

#define X(n) __typeof__(&n) x2p_##n;
X2_GL_FUNCS(X)
#undef X

void x2_gl_load(void *(*get_proc)(const char *name)) {
#define X(n) x2p_##n = (__typeof__(x2p_##n))get_proc(#n);
    X2_GL_FUNCS(X)
#undef X
}
#endif
