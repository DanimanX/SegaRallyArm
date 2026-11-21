/* Dynamic OpenGL ES 2.0 / Desktop OpenGL function loader via SDL2.
 * Eliminates direct link against libGL or libGLESv2, allowing portable execution
 * across Mali-G31 (libmali/EGL), Panfrost, Mesa, and desktop Linux/macOS.
 */
#ifndef PLATFORM_ARM64_GL_FUNCS_H
#define PLATFORM_ARM64_GL_FUNCS_H

#include <SDL.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef int            GLint;
typedef unsigned int   GLuint;
typedef int            GLsizei;
typedef float          GLfloat;
typedef char           GLchar;
typedef unsigned char  GLubyte;
typedef ptrdiff_t      GLintptr;
typedef ptrdiff_t      GLsizeiptr;

#define GL_FALSE                       0
#define GL_TRUE                        1
#define GL_DEPTH_BUFFER_BIT            0x00000100
#define GL_STENCIL_BUFFER_BIT          0x00000400
#define GL_COLOR_BUFFER_BIT            0x00004000

#define GL_TRIANGLES                   0x0004
#define GL_TRIANGLE_STRIP              0x0005
#define GL_EQUAL                       0x0202
#define GL_LEQUAL                      0x0203
#define GL_LESS                        0x0201
#define GL_GEQUAL                      0x0206
#define GL_SRC_ALPHA                   0x0302
#define GL_ONE_MINUS_SRC_ALPHA         0x0303

#define GL_KEEP                        0x1E00
#define GL_INCR                        0x1E02

#define GL_CULL_FACE                   0x0B44
#define GL_DEPTH_TEST                  0x0B71
#define GL_STENCIL_TEST                0x0B90
#define GL_BLEND                       0x0BE2
#define GL_SCISSOR_TEST                0x0C11
#define GL_UNPACK_ALIGNMENT            0x0CF5
#define GL_TEXTURE_2D                  0x0DE1

#define GL_UNSIGNED_BYTE               0x1401
#define GL_UNSIGNED_SHORT              0x1403
#define GL_UNSIGNED_INT                0x1405
#define GL_FLOAT                       0x1406

#define GL_LUMINANCE                   0x1909
#define GL_RGBA                        0x1908
#define GL_STENCIL_BITS                0x0D57

#define GL_VENDOR                      0x1F00
#define GL_RENDERER                    0x1F01
#define GL_VERSION                     0x1F02

#define GL_NEAREST                     0x2600
#define GL_LINEAR                      0x2601
#define GL_TEXTURE_MAG_FILTER          0x2800
#define GL_TEXTURE_MIN_FILTER          0x2801
#define GL_TEXTURE_WRAP_S              0x2802
#define GL_TEXTURE_WRAP_T              0x2803
#define GL_CLAMP_TO_EDGE               0x812F

#define GL_TEXTURE0                    0x84C0
#define GL_TEXTURE1                    0x84C1

#define GL_ARRAY_BUFFER                0x8892
#define GL_ELEMENT_ARRAY_BUFFER        0x8893
#define GL_STREAM_DRAW                 0x88E0
#define GL_STATIC_DRAW                 0x88E4
#define GL_DYNAMIC_DRAW                0x88E8

#define GL_FRAGMENT_SHADER             0x8B30
#define GL_VERTEX_SHADER               0x8B31
#define GL_COMPILE_STATUS              0x8B81
#define GL_LINK_STATUS                 0x8B82

/* Function pointer types */
typedef void (*PFN_glViewport)(GLint, GLint, GLsizei, GLsizei);
typedef void (*PFN_glScissor)(GLint, GLint, GLsizei, GLsizei);
typedef void (*PFN_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFN_glClear)(GLbitfield);
typedef void (*PFN_glEnable)(GLenum);
typedef void (*PFN_glDisable)(GLenum);
typedef void (*PFN_glDepthFunc)(GLenum);
typedef void (*PFN_glDepthMask)(GLboolean);
typedef void (*PFN_glBlendFunc)(GLenum, GLenum);
typedef void (*PFN_glCullFace)(GLenum);
typedef void (*PFN_glColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
typedef void (*PFN_glStencilFunc)(GLenum, GLint, GLuint);
typedef void (*PFN_glStencilOp)(GLenum, GLenum, GLenum);
typedef void (*PFN_glStencilMask)(GLuint);
typedef void (*PFN_glGetIntegerv)(GLenum, GLint *);
typedef const GLubyte *(*PFN_glGetString)(GLenum);
typedef void (*PFN_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
typedef GLenum (*PFN_glGetError)(void);

typedef void (*PFN_glGenTextures)(GLsizei, GLuint *);
typedef void (*PFN_glDeleteTextures)(GLsizei, const GLuint *);
typedef void (*PFN_glBindTexture)(GLenum, GLuint);
typedef void (*PFN_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (*PFN_glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
typedef void (*PFN_glTexParameteri)(GLenum, GLenum, GLint);
typedef void (*PFN_glPixelStorei)(GLenum, GLint);
typedef void (*PFN_glActiveTexture)(GLenum);

typedef GLuint (*PFN_glCreateShader)(GLenum);
typedef void (*PFN_glDeleteShader)(GLuint);
typedef void (*PFN_glShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void (*PFN_glCompileShader)(GLuint);
typedef void (*PFN_glGetShaderiv)(GLuint, GLenum, GLint *);
typedef void (*PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);

typedef GLuint (*PFN_glCreateProgram)(void);
typedef void (*PFN_glDeleteProgram)(GLuint);
typedef void (*PFN_glAttachShader)(GLuint, GLuint);
typedef void (*PFN_glLinkProgram)(GLuint);
typedef void (*PFN_glUseProgram)(GLuint);
typedef void (*PFN_glGetProgramiv)(GLuint, GLenum, GLint *);
typedef void (*PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);

typedef GLint (*PFN_glGetUniformLocation)(GLuint, const GLchar *);
typedef GLint (*PFN_glGetAttribLocation)(GLuint, const GLchar *);
typedef void (*PFN_glUniform1i)(GLint, GLint);
typedef void (*PFN_glUniform1f)(GLint, GLfloat);
typedef void (*PFN_glUniform2f)(GLint, GLfloat, GLfloat);
typedef void (*PFN_glUniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFN_glUniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat *);

typedef void (*PFN_glGenBuffers)(GLsizei, GLuint *);
typedef void (*PFN_glDeleteBuffers)(GLsizei, const GLuint *);
typedef void (*PFN_glBindBuffer)(GLenum, GLuint);
typedef void (*PFN_glBufferData)(GLenum, GLsizeiptr, const void *, GLenum);
typedef void (*PFN_glBufferSubData)(GLenum, GLintptr, GLsizeiptr, const void *);

typedef void (*PFN_glEnableVertexAttribArray)(GLuint);
typedef void (*PFN_glDisableVertexAttribArray)(GLuint);
typedef void (*PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);

typedef void (*PFN_glDrawArrays)(GLenum, GLint, GLsizei);
typedef void (*PFN_glDrawElements)(GLenum, GLsizei, GLenum, const void *);

/* Global function table */
typedef struct arm64_gl_api {
    PFN_glViewport Viewport;
    PFN_glScissor Scissor;
    PFN_glClearColor ClearColor;
    PFN_glClear Clear;
    PFN_glEnable Enable;
    PFN_glDisable Disable;
    PFN_glDepthFunc DepthFunc;
    PFN_glDepthMask DepthMask;
    PFN_glBlendFunc BlendFunc;
    PFN_glCullFace CullFace;
    PFN_glColorMask ColorMask;
    PFN_glStencilFunc StencilFunc;
    PFN_glStencilOp StencilOp;
    PFN_glStencilMask StencilMask;
    PFN_glGetIntegerv GetIntegerv;
    PFN_glGetString GetString;
    PFN_glReadPixels ReadPixels;
    PFN_glGetError GetError;

    PFN_glGenTextures GenTextures;
    PFN_glDeleteTextures DeleteTextures;
    PFN_glBindTexture BindTexture;
    PFN_glTexImage2D TexImage2D;
    PFN_glTexSubImage2D TexSubImage2D;
    PFN_glTexParameteri TexParameteri;
    PFN_glPixelStorei PixelStorei;
    PFN_glActiveTexture ActiveTexture;

    PFN_glCreateShader CreateShader;
    PFN_glDeleteShader DeleteShader;
    PFN_glShaderSource ShaderSource;
    PFN_glCompileShader CompileShader;
    PFN_glGetShaderiv GetShaderiv;
    PFN_glGetShaderInfoLog GetShaderInfoLog;

    PFN_glCreateProgram CreateProgram;
    PFN_glDeleteProgram DeleteProgram;
    PFN_glAttachShader AttachShader;
    PFN_glLinkProgram LinkProgram;
    PFN_glUseProgram UseProgram;
    PFN_glGetProgramiv GetProgramiv;
    PFN_glGetProgramInfoLog GetProgramInfoLog;

    PFN_glGetUniformLocation GetUniformLocation;
    PFN_glGetAttribLocation GetAttribLocation;
    PFN_glUniform1i Uniform1i;
    PFN_glUniform1f Uniform1f;
    PFN_glUniform2f Uniform2f;
    PFN_glUniform4f Uniform4f;
    PFN_glUniformMatrix4fv UniformMatrix4fv;

    PFN_glGenBuffers GenBuffers;
    PFN_glDeleteBuffers DeleteBuffers;
    PFN_glBindBuffer BindBuffer;
    PFN_glBufferData BufferData;
    PFN_glBufferSubData BufferSubData;

    PFN_glEnableVertexAttribArray EnableVertexAttribArray;
    PFN_glDisableVertexAttribArray DisableVertexAttribArray;
    PFN_glVertexAttribPointer VertexAttribPointer;

    PFN_glDrawArrays DrawArrays;
    PFN_glDrawElements DrawElements;
} arm64_gl_api_t;

extern arm64_gl_api_t arm64_gl;

int arm64_gl_load_all(void);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_ARM64_GL_FUNCS_H */
