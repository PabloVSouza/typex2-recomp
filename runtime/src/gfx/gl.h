/* GL headers: desktop GL 3.3+ core on macOS/Linux, GLES 3.0 on Android/iOS.
 * Both are linked directly (no loader needed on these platforms). */
#pragma once
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <OpenGLES/ES3/gl.h>
#define X2_GLES 1
#else
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>
#endif
#elif defined(__ANDROID__)
#include <GLES3/gl3.h>
#define X2_GLES 1
#else
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_UNSIGNED_SHORT_1_5_5_5_REV
#define GL_UNSIGNED_SHORT_1_5_5_5_REV 0x8366
#define GL_UNSIGNED_SHORT_4_4_4_4_REV 0x8365
#endif
#ifndef X2_GLES
#define glClearDepthf(d) glClearDepth(d)
#endif
