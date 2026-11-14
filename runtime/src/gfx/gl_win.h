/* GL entry points through pointers on Windows, where opengl32.dll exports GL 1.1 only.
 * The list: every gl* call in runtime/src, regenerated with
 *   every gl... name called with a parenthesis in the .c files of runtime/src and runtime/src/gfx, sorted, unique
 * (plus glClearDepth, used through glClearDepthf). Filled by x2_gl_load once a context exists. */
#pragma once
#define X2_GL_FUNCS(X) \
    X(glActiveTexture) \
    X(glAttachShader) \
    X(glBeginQuery) \
    X(glBindBuffer) \
    X(glBindFramebuffer) \
    X(glBindSampler) \
    X(glBindTexture) \
    X(glBindVertexArray) \
    X(glBlendColor) \
    X(glBlendEquation) \
    X(glBlendEquationSeparate) \
    X(glBlendFunc) \
    X(glBlendFuncSeparate) \
    X(glBlitFramebuffer) \
    X(glBufferData) \
    X(glBufferSubData) \
    X(glCheckFramebufferStatus) \
    X(glClear) \
    X(glClearColor) \
    X(glClearDepth) \
    X(glClearStencil) \
    X(glColorMask) \
    X(glCompileShader) \
    X(glCompressedTexImage2D) \
    X(glCompressedTexSubImage2D) \
    X(glCreateProgram) \
    X(glCreateShader) \
    X(glCullFace) \
    X(glDeleteBuffers) \
    X(glDeleteFramebuffers) \
    X(glDeleteProgram) \
    X(glDeleteShader) \
    X(glDeleteTextures) \
    X(glDepthFunc) \
    X(glDepthMask) \
    X(glDepthRange) \
    X(glDepthRangef) \
    X(glDisable) \
    X(glDisableVertexAttribArray) \
    X(glDrawArrays) \
    X(glDrawBuffers) \
    X(glDrawElements) \
    X(glDrawElementsBaseVertex) \
    X(glEnable) \
    X(glEnableVertexAttribArray) \
    X(glEndQuery) \
    X(glFramebufferTexture2D) \
    X(glFrontFace) \
    X(glGenBuffers) \
    X(glGenFramebuffers) \
    X(glGenQueries) \
    X(glGenSamplers) \
    X(glGenTextures) \
    X(glGenVertexArrays) \
    X(glGetCompressedTexImage) \
    X(glGetError) \
    X(glGetIntegerv) \
    X(glGetProgramBinary) \
    X(glGetProgramInfoLog) \
    X(glGetProgramiv) \
    X(glGetQueryObjectuiv) \
    X(glGetShaderInfoLog) \
    X(glGetShaderiv) \
    X(glGetString) \
    X(glGetStringi) \
    X(glGetTexImage) \
    X(glGetTexLevelParameteriv) \
    X(glGetUniformLocation) \
    X(glIsEnabled) \
    X(glLinkProgram) \
    X(glMapBufferRange) \
    X(glPixelStorei) \
    X(glProgramBinary) \
    X(glProgramParameteri) \
    X(glReadPixels) \
    X(glSamplerParameterfv) \
    X(glSamplerParameteri) \
    X(glScissor) \
    X(glShaderSource) \
    X(glStencilFunc) \
    X(glStencilMask) \
    X(glStencilOp) \
    X(glTexImage2D) \
    X(glTexParameteri) \
    X(glTexSubImage2D) \
    X(glUniform1f) \
    X(glUniform1i) \
    X(glUniform1iv) \
    X(glUniform2f) \
    X(glUniform4f) \
    X(glUniform4fv) \
    X(glUniform4iv) \
    X(glUnmapBuffer) \
    X(glUseProgram) \
    X(glVertexAttrib4f) \
    X(glVertexAttribPointer) \
    X(glViewport)

#ifndef X2_GL_LOADER
#define X(n) extern __typeof__(&n) x2p_##n;
X2_GL_FUNCS(X)
#undef X
#define glActiveTexture x2p_glActiveTexture
#define glAttachShader x2p_glAttachShader
#define glBeginQuery x2p_glBeginQuery
#define glBindBuffer x2p_glBindBuffer
#define glBindFramebuffer x2p_glBindFramebuffer
#define glBindSampler x2p_glBindSampler
#define glBindTexture x2p_glBindTexture
#define glBindVertexArray x2p_glBindVertexArray
#define glBlendColor x2p_glBlendColor
#define glBlendEquation x2p_glBlendEquation
#define glBlendEquationSeparate x2p_glBlendEquationSeparate
#define glBlendFunc x2p_glBlendFunc
#define glBlendFuncSeparate x2p_glBlendFuncSeparate
#define glBlitFramebuffer x2p_glBlitFramebuffer
#define glBufferData x2p_glBufferData
#define glBufferSubData x2p_glBufferSubData
#define glCheckFramebufferStatus x2p_glCheckFramebufferStatus
#define glClear x2p_glClear
#define glClearColor x2p_glClearColor
#define glClearDepth x2p_glClearDepth
#define glClearStencil x2p_glClearStencil
#define glColorMask x2p_glColorMask
#define glCompileShader x2p_glCompileShader
#define glCompressedTexImage2D x2p_glCompressedTexImage2D
#define glCompressedTexSubImage2D x2p_glCompressedTexSubImage2D
#define glCreateProgram x2p_glCreateProgram
#define glCreateShader x2p_glCreateShader
#define glCullFace x2p_glCullFace
#define glDeleteBuffers x2p_glDeleteBuffers
#define glDeleteFramebuffers x2p_glDeleteFramebuffers
#define glDeleteProgram x2p_glDeleteProgram
#define glDeleteShader x2p_glDeleteShader
#define glDeleteTextures x2p_glDeleteTextures
#define glDepthFunc x2p_glDepthFunc
#define glDepthMask x2p_glDepthMask
#define glDepthRange x2p_glDepthRange
#define glDepthRangef x2p_glDepthRangef
#define glDisable x2p_glDisable
#define glDisableVertexAttribArray x2p_glDisableVertexAttribArray
#define glDrawArrays x2p_glDrawArrays
#define glDrawBuffers x2p_glDrawBuffers
#define glDrawElements x2p_glDrawElements
#define glDrawElementsBaseVertex x2p_glDrawElementsBaseVertex
#define glEnable x2p_glEnable
#define glEnableVertexAttribArray x2p_glEnableVertexAttribArray
#define glEndQuery x2p_glEndQuery
#define glFramebufferTexture2D x2p_glFramebufferTexture2D
#define glFrontFace x2p_glFrontFace
#define glGenBuffers x2p_glGenBuffers
#define glGenFramebuffers x2p_glGenFramebuffers
#define glGenQueries x2p_glGenQueries
#define glGenSamplers x2p_glGenSamplers
#define glGenTextures x2p_glGenTextures
#define glGenVertexArrays x2p_glGenVertexArrays
#define glGetCompressedTexImage x2p_glGetCompressedTexImage
#define glGetError x2p_glGetError
#define glGetIntegerv x2p_glGetIntegerv
#define glGetProgramBinary x2p_glGetProgramBinary
#define glGetProgramInfoLog x2p_glGetProgramInfoLog
#define glGetProgramiv x2p_glGetProgramiv
#define glGetQueryObjectuiv x2p_glGetQueryObjectuiv
#define glGetShaderInfoLog x2p_glGetShaderInfoLog
#define glGetShaderiv x2p_glGetShaderiv
#define glGetString x2p_glGetString
#define glGetStringi x2p_glGetStringi
#define glGetTexImage x2p_glGetTexImage
#define glGetTexLevelParameteriv x2p_glGetTexLevelParameteriv
#define glGetUniformLocation x2p_glGetUniformLocation
#define glIsEnabled x2p_glIsEnabled
#define glLinkProgram x2p_glLinkProgram
#define glMapBufferRange x2p_glMapBufferRange
#define glPixelStorei x2p_glPixelStorei
#define glProgramBinary x2p_glProgramBinary
#define glProgramParameteri x2p_glProgramParameteri
#define glReadPixels x2p_glReadPixels
#define glSamplerParameterfv x2p_glSamplerParameterfv
#define glSamplerParameteri x2p_glSamplerParameteri
#define glScissor x2p_glScissor
#define glShaderSource x2p_glShaderSource
#define glStencilFunc x2p_glStencilFunc
#define glStencilMask x2p_glStencilMask
#define glStencilOp x2p_glStencilOp
#define glTexImage2D x2p_glTexImage2D
#define glTexParameteri x2p_glTexParameteri
#define glTexSubImage2D x2p_glTexSubImage2D
#define glUniform1f x2p_glUniform1f
#define glUniform1i x2p_glUniform1i
#define glUniform1iv x2p_glUniform1iv
#define glUniform2f x2p_glUniform2f
#define glUniform4f x2p_glUniform4f
#define glUniform4fv x2p_glUniform4fv
#define glUniform4iv x2p_glUniform4iv
#define glUnmapBuffer x2p_glUnmapBuffer
#define glUseProgram x2p_glUseProgram
#define glVertexAttrib4f x2p_glVertexAttrib4f
#define glVertexAttribPointer x2p_glVertexAttribPointer
#define glViewport x2p_glViewport
#endif
void x2_gl_load(void *(*get_proc)(const char *name));
