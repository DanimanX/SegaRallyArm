#include "gl_funcs.h"
#include <stdio.h>

arm64_gl_api_t arm64_gl;

int arm64_gl_load_all(void)
{
    static int loaded = 0;
    if (loaded) return 1;

#define LOAD_PROC(fn, name) \
    arm64_gl.fn = (PFN_##name)SDL_GL_GetProcAddress(#name); \
    if (!arm64_gl.fn) { \
        fprintf(stderr, "gl_funcs: warning: failed to resolve %s\n", #name); \
    }

    LOAD_PROC(Viewport, glViewport);
    LOAD_PROC(Scissor, glScissor);
    LOAD_PROC(ClearColor, glClearColor);
    LOAD_PROC(ClearStencil, glClearStencil);
    LOAD_PROC(Clear, glClear);
    LOAD_PROC(Enable, glEnable);
    LOAD_PROC(Disable, glDisable);
    LOAD_PROC(DepthFunc, glDepthFunc);
    LOAD_PROC(DepthMask, glDepthMask);
    LOAD_PROC(PolygonOffset, glPolygonOffset);
    LOAD_PROC(BlendFunc, glBlendFunc);
    LOAD_PROC(CullFace, glCullFace);
    LOAD_PROC(ColorMask, glColorMask);
    LOAD_PROC(StencilFunc, glStencilFunc);
    LOAD_PROC(StencilOp, glStencilOp);
    LOAD_PROC(StencilMask, glStencilMask);
    LOAD_PROC(GetIntegerv, glGetIntegerv);
    LOAD_PROC(GetString, glGetString);
    LOAD_PROC(ReadPixels, glReadPixels);
    LOAD_PROC(GetError, glGetError);

    LOAD_PROC(GenTextures, glGenTextures);
    LOAD_PROC(DeleteTextures, glDeleteTextures);
    LOAD_PROC(BindTexture, glBindTexture);
    LOAD_PROC(TexImage2D, glTexImage2D);
    LOAD_PROC(TexSubImage2D, glTexSubImage2D);
    LOAD_PROC(TexParameteri, glTexParameteri);
    LOAD_PROC(PixelStorei, glPixelStorei);
    LOAD_PROC(ActiveTexture, glActiveTexture);

    LOAD_PROC(CreateShader, glCreateShader);
    LOAD_PROC(DeleteShader, glDeleteShader);
    LOAD_PROC(ShaderSource, glShaderSource);
    LOAD_PROC(CompileShader, glCompileShader);
    LOAD_PROC(GetShaderiv, glGetShaderiv);
    LOAD_PROC(GetShaderInfoLog, glGetShaderInfoLog);

    LOAD_PROC(CreateProgram, glCreateProgram);
    LOAD_PROC(DeleteProgram, glDeleteProgram);
    LOAD_PROC(AttachShader, glAttachShader);
    LOAD_PROC(LinkProgram, glLinkProgram);
    LOAD_PROC(UseProgram, glUseProgram);
    LOAD_PROC(GetProgramiv, glGetProgramiv);
    LOAD_PROC(GetProgramInfoLog, glGetProgramInfoLog);

    LOAD_PROC(GetUniformLocation, glGetUniformLocation);
    LOAD_PROC(GetAttribLocation, glGetAttribLocation);
    LOAD_PROC(Uniform1i, glUniform1i);
    LOAD_PROC(Uniform1f, glUniform1f);
    LOAD_PROC(Uniform2f, glUniform2f);
    LOAD_PROC(Uniform4f, glUniform4f);
    LOAD_PROC(UniformMatrix4fv, glUniformMatrix4fv);

    LOAD_PROC(GenBuffers, glGenBuffers);
    LOAD_PROC(DeleteBuffers, glDeleteBuffers);
    LOAD_PROC(BindBuffer, glBindBuffer);
    LOAD_PROC(BufferData, glBufferData);
    LOAD_PROC(BufferSubData, glBufferSubData);

    LOAD_PROC(EnableVertexAttribArray, glEnableVertexAttribArray);
    LOAD_PROC(DisableVertexAttribArray, glDisableVertexAttribArray);
    LOAD_PROC(VertexAttribPointer, glVertexAttribPointer);

    LOAD_PROC(DrawArrays, glDrawArrays);
    LOAD_PROC(DrawElements, glDrawElements);

    LOAD_PROC(GenFramebuffers, glGenFramebuffers);
    LOAD_PROC(DeleteFramebuffers, glDeleteFramebuffers);
    LOAD_PROC(BindFramebuffer, glBindFramebuffer);
    LOAD_PROC(FramebufferTexture2D, glFramebufferTexture2D);
    LOAD_PROC(GenRenderbuffers, glGenRenderbuffers);
    LOAD_PROC(DeleteRenderbuffers, glDeleteRenderbuffers);
    LOAD_PROC(BindRenderbuffer, glBindRenderbuffer);
    LOAD_PROC(RenderbufferStorage, glRenderbufferStorage);
    LOAD_PROC(FramebufferRenderbuffer, glFramebufferRenderbuffer);
    LOAD_PROC(CheckFramebufferStatus, glCheckFramebufferStatus);

#undef LOAD_PROC

    loaded = 1;
    return 1;
}
