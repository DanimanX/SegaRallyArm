#include "gpu_gles.h"
#include "gl_funcs.h"
#include "model2_geo_tex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SYS24_W 496
#define SYS24_H 384
#define LUT_CACHE_CAP 512

typedef struct {
    uint16_t colorbase;
    uint16_t lumabase;
    uint8_t sheet;
    uint8_t flags;
    uint8_t luma;
    GLuint tex;
} lut_entry_t;

static int g_ready = 0;
static arm64_aspect_t g_aspect = ASPECT_4_3_CENTERED;
static int g_win_w = 720;
static int g_win_h = 480;

/* Shaders and programs */
static GLuint g_geo_prog = 0;
static GLuint g_tile_prog = 0;

/* Uniform locations for Geo Program */
static GLint g_u_mvp = -1;
static GLint g_u_sheet = -1;
static GLint g_u_lut = -1;
static GLint g_u_cutout = -1;
static GLint g_u_patch = -1;
static GLint g_u_mirror = -1;

/* Attribute locations for Geo Program */
static GLint g_a_pos = -1;
static GLint g_a_uv = -1;

/* Uniform and attribute locations for Tile Program */
static GLint g_u_tile_tex = -1;
static GLint g_a_tile_pos = -1;
static GLint g_a_tile_uv = -1;

/* Textures */
static GLuint g_sheet_tex[2] = {0, 0};
static GLuint g_tile_tex = 0;
static uint32_t *g_tile_rgba_buf = NULL;

/* VBO buffers */
static GLuint g_geo_vbo = 0;
static GLuint g_tile_vbo = 0;

/* Palette LUT Cache */
static lut_entry_t g_lut_cache[LUT_CACHE_CAP];
static unsigned g_lut_n = 0;
static unsigned g_lut_clock = 0;
static uint32_t g_sheets_gen = 0;
static uint32_t g_lut_sig = 0;

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint s = arm64_gl.CreateShader(type);
    arm64_gl.ShaderSource(s, 1, &src, NULL);
    arm64_gl.CompileShader(s);

    GLint ok = 0;
    arm64_gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        arm64_gl.GetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "gpu_gles: shader compilation failed:\n%s\n", log);
        arm64_gl.DeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint create_program(const char *vs_src, const char *fs_src)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return 0;

    GLuint prog = arm64_gl.CreateProgram();
    arm64_gl.AttachShader(prog, vs);
    arm64_gl.AttachShader(prog, fs);
    arm64_gl.LinkProgram(prog);

    arm64_gl.DeleteShader(vs);
    arm64_gl.DeleteShader(fs);

    GLint ok = 0;
    arm64_gl.GetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        arm64_gl.GetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "gpu_gles: program link failed:\n%s\n", log);
        arm64_gl.DeleteProgram(prog);
        return 0;
    }
    return prog;
}

static void lut_cache_clear(void)
{
    for (unsigned i = 0; i < g_lut_n; i++) {
        arm64_gl.DeleteTextures(1, &g_lut_cache[i].tex);
    }
    g_lut_n = 0;
    g_lut_clock = 0;
}

static GLuint lut_cache_get(const model2_geo_tri_mat_t *m)
{
    for (unsigned i = 0; i < g_lut_n; i++) {
        if (g_lut_cache[i].colorbase == m->colorbase &&
            g_lut_cache[i].lumabase == m->lumabase &&
            g_lut_cache[i].sheet == m->sheet &&
            g_lut_cache[i].flags == m->flags &&
            g_lut_cache[i].luma == m->luma) {
            return g_lut_cache[i].tex;
        }
    }

    uint8_t lut[16 * 4];
    model2_palette_build_texel_lut(m->colorbase, m->lumabase, m->luma,
                                   (m->flags & MODEL2_GEO_TEX_CUTOUT) ? 1 : 0, lut);

    GLuint tex = 0;
    arm64_gl.GenTextures(1, &tex);
    arm64_gl.BindTexture(GL_TEXTURE_2D, tex);
    arm64_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    arm64_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    arm64_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    arm64_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    arm64_gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    arm64_gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, lut);

    lut_entry_t *ent = NULL;
    if (g_lut_n < LUT_CACHE_CAP) {
        ent = &g_lut_cache[g_lut_n++];
    } else {
        unsigned slot = g_lut_clock++ % LUT_CACHE_CAP;
        arm64_gl.DeleteTextures(1, &g_lut_cache[slot].tex);
        ent = &g_lut_cache[slot];
    }
    ent->colorbase = m->colorbase;
    ent->lumabase = m->lumabase;
    ent->sheet = m->sheet;
    ent->flags = m->flags;
    ent->luma = m->luma;
    ent->tex = tex;
    return tex;
}

static void upload_sheets(void)
{
    uint32_t gen = model2_tex_sheets_dirty_gen();
    if (g_sheets_gen == gen) return;

    uint8_t *r8 = (uint8_t *)malloc(2048 * 1024);
    if (!r8) return;

    for (unsigned s = 0; s < 2; s++) {
        const uint32_t *bank = model2_tex_sheet_bank(s);
        if (!bank) continue;
        model2_decode_sheet_indices(bank, r8);
        arm64_gl.BindTexture(GL_TEXTURE_2D, g_sheet_tex[s]);
        arm64_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        arm64_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        arm64_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        arm64_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        arm64_gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        arm64_gl.TexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 2048, 1024, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, r8);
    }
    free(r8);
    g_sheets_gen = gen;
}

int arm64_gpu_gles_init(int win_w, int win_h)
{
    if (g_ready) return 0;
    if (!arm64_gl_load_all()) {
        fprintf(stderr, "gpu_gles: failed to load OpenGL ES 2.0 functions\n");
        return -1;
    }

    g_win_w = win_w;
    g_win_h = win_h;

    /* Model 2A 3D Geometry GLSL ES 1.00 Shader */
    static const char *geo_vs =
        "precision highp float;\n"
        "attribute vec3 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "uniform mat4 u_mvp;\n"
        "varying vec2 v_uv;\n"
        "void main() {\n"
        "    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
        "    v_uv = a_uv;\n"
        "}\n";

    static const char *geo_fs =
        "precision mediump float;\n"
        "varying vec2 v_uv;\n"
        "uniform sampler2D uSheet;\n"
        "uniform sampler2D uLut;\n"
        "uniform float uCutout;\n"
        "uniform vec4 uPatch;\n"
        "uniform vec2 uMirror;\n"
        "float wrap_coord(float pix, float size, float mirror) {\n"
        "    float s = max(size, 1.0);\n"
        "    float cell = floor(pix / s);\n"
        "    float local = pix - cell * s;\n"
        "    if (mirror > 0.5 && mod(cell, 2.0) >= 1.0)\n"
        "        local = s - 1.0 - local;\n"
        "    local = local - floor(local / s) * s;\n"
        "    if (local < 0.0) local += s;\n"
        "    return floor(local);\n"
        "}\n"
        "void main() {\n"
        "    float u0 = wrap_coord(v_uv.x, uPatch.z, uMirror.x);\n"
        "    float v0 = wrap_coord(v_uv.y, uPatch.w, uMirror.y);\n"
        "    vec2 st = vec2((uPatch.x + u0 + 0.5) / 2048.0,\n"
        "                   (uPatch.y + v0 + 0.5) / 1024.0);\n"
        "    float t = floor(texture2D(uSheet, st).r * 255.0 + 0.5);\n"
        "    vec4 c = texture2D(uLut, vec2((t + 0.5) / 16.0, 0.5));\n"
        "    if (uCutout > 0.5 && c.a < 0.5) discard;\n"
        "    gl_FragColor = vec4(c.rgb, 1.0);\n"
        "}\n";

    g_geo_prog = create_program(geo_vs, geo_fs);
    if (!g_geo_prog) return -1;

    g_u_mvp = arm64_gl.GetUniformLocation(g_geo_prog, "u_mvp");
    g_u_sheet = arm64_gl.GetUniformLocation(g_geo_prog, "uSheet");
    g_u_lut = arm64_gl.GetUniformLocation(g_geo_prog, "uLut");
    g_u_cutout = arm64_gl.GetUniformLocation(g_geo_prog, "uCutout");
    g_u_patch = arm64_gl.GetUniformLocation(g_geo_prog, "uPatch");
    g_u_mirror = arm64_gl.GetUniformLocation(g_geo_prog, "uMirror");
    g_a_pos = arm64_gl.GetAttribLocation(g_geo_prog, "a_pos");
    g_a_uv = arm64_gl.GetAttribLocation(g_geo_prog, "a_uv");

    /* System 24 2D Overlay Quad Shader */
    static const char *tile_vs =
        "attribute vec2 a_tile_pos;\n"
        "attribute vec2 a_tile_uv;\n"
        "varying vec2 v_tile_uv;\n"
        "void main() {\n"
        "    gl_Position = vec4(a_tile_pos, 0.0, 1.0);\n"
        "    v_tile_uv = a_tile_uv;\n"
        "}\n";

    static const char *tile_fs =
        "precision mediump float;\n"
        "varying vec2 v_tile_uv;\n"
        "uniform sampler2D uTileTex;\n"
        "void main() {\n"
        "    gl_FragColor = texture2D(uTileTex, v_tile_uv);\n"
        "}\n";

    g_tile_prog = create_program(tile_vs, tile_fs);
    if (!g_tile_prog) return -1;

    g_u_tile_tex = arm64_gl.GetUniformLocation(g_tile_prog, "uTileTex");
    g_a_tile_pos = arm64_gl.GetAttribLocation(g_tile_prog, "a_tile_pos");
    g_a_tile_uv = arm64_gl.GetAttribLocation(g_tile_prog, "a_tile_uv");

    /* Allocate textures */
    arm64_gl.GenTextures(2, g_sheet_tex);
    arm64_gl.GenTextures(1, &g_tile_tex);
    arm64_gl.BindTexture(GL_TEXTURE_2D, g_tile_tex);
    arm64_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    arm64_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    arm64_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    arm64_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    /* Allocate buffers */
    arm64_gl.GenBuffers(1, &g_geo_vbo);
    arm64_gl.GenBuffers(1, &g_tile_vbo);

    /* Setup fullscreen tile quad geometry (NDC -1..+1) */
    static const float tile_quad[16] = {
        /* X, Y, U, V */
        -1.0f,  1.0f,  0.0f, 0.0f,
        -1.0f, -1.0f,  0.0f, 1.0f,
         1.0f,  1.0f,  1.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 1.0f
    };
    arm64_gl.BindBuffer(GL_ARRAY_BUFFER, g_tile_vbo);
    arm64_gl.BufferData(GL_ARRAY_BUFFER, sizeof(tile_quad), tile_quad, GL_STATIC_DRAW);

    g_tile_rgba_buf = (uint32_t *)malloc(SYS24_W * SYS24_H * sizeof(uint32_t));

    g_ready = 1;
    printf("gpu_gles: initialized Mali-G31 GLES 2.0 pipeline (%dx%d)\n", win_w, win_h);
    return 0;
}

void arm64_gpu_gles_shutdown(void)
{
    if (!g_ready) return;
    if (g_geo_prog) arm64_gl.DeleteProgram(g_geo_prog);
    if (g_tile_prog) arm64_gl.DeleteProgram(g_tile_prog);
    if (g_sheet_tex[0]) arm64_gl.DeleteTextures(2, g_sheet_tex);
    if (g_tile_tex) arm64_gl.DeleteTextures(1, &g_tile_tex);
    if (g_geo_vbo) arm64_gl.DeleteBuffers(1, &g_geo_vbo);
    if (g_tile_vbo) arm64_gl.DeleteBuffers(1, &g_tile_vbo);
    lut_cache_clear();
    free(g_tile_rgba_buf);
    g_tile_rgba_buf = NULL;
    g_ready = 0;
}

void arm64_gpu_gles_set_aspect(arm64_aspect_t mode)
{
    g_aspect = mode;
}

arm64_aspect_t arm64_gpu_gles_get_aspect(void)
{
    return g_aspect;
}

static void calculate_viewport(int win_w, int win_h, int *vx, int *vy, int *vw, int *vh)
{
    if (g_aspect == ASPECT_FULLSCREEN) {
        *vx = 0; *vy = 0; *vw = win_w; *vh = win_h;
        return;
    }

    /* 4:3 Centered Arcade Aspect (e.g. 620x480 inside 720x480 RG34XX panel) */
    float target_ratio = 4.0f / 3.0f;
    int w = (int)(win_h * target_ratio);
    if (w > win_w) {
        w = win_w;
        int h = (int)(win_w / target_ratio);
        *vx = 0;
        *vy = (win_h - h) / 2;
        *vw = w;
        *vh = h;
    } else {
        *vx = (win_w - w) / 2;
        *vy = 0;
        *vw = w;
        *vh = win_h;
    }
}

void arm64_gpu_gles_begin_frame(int display_w, int display_h)
{
    g_win_w = display_w;
    g_win_h = display_h;

    int vx, vy, vw, vh;
    calculate_viewport(display_w, display_h, &vx, &vy, &vw, &vh);

    arm64_gl.Viewport(0, 0, display_w, display_h);
    arm64_gl.ClearColor(0.02f, 0.02f, 0.03f, 1.0f);
    arm64_gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    arm64_gl.Viewport(vx, vy, vw, vh);
    arm64_gl.Scissor(vx, vy, vw, vh);
    arm64_gl.Enable(GL_SCISSOR_TEST);
}

void arm64_gpu_gles_end_frame(void)
{
    arm64_gl.Disable(GL_SCISSOR_TEST);
}

void arm64_gpu_gles_draw_tiles(const uint32_t *src_argb, int transparent)
{
    if (!g_ready || !src_argb) return;

    /* Convert ARGB to RGBA with alpha punch for transparency */
    for (int i = 0; i < SYS24_W * SYS24_H; i++) {
        uint32_t p = src_argb[i];
        uint8_t a = (p >> 24) & 0xFF;
        uint8_t r = (p >> 16) & 0xFF;
        uint8_t g = (p >> 8) & 0xFF;
        uint8_t b = p & 0xFF;
        if (transparent && (p & 0x00FFFFFF) == 0) {
            a = 0;
        }
        g_tile_rgba_buf[i] = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
    }

    arm64_gl.ActiveTexture(GL_TEXTURE0);
    arm64_gl.BindTexture(GL_TEXTURE_2D, g_tile_tex);
    arm64_gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SYS24_W, SYS24_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_tile_rgba_buf);

    arm64_gl.UseProgram(g_tile_prog);
    arm64_gl.Uniform1i(g_u_tile_tex, 0);

    if (transparent) {
        arm64_gl.Enable(GL_BLEND);
        arm64_gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        arm64_gl.Disable(GL_BLEND);
    }

    arm64_gl.Disable(GL_DEPTH_TEST);
    arm64_gl.DepthMask(GL_FALSE);

    arm64_gl.BindBuffer(GL_ARRAY_BUFFER, g_tile_vbo);
    arm64_gl.EnableVertexAttribArray(g_a_tile_pos);
    arm64_gl.EnableVertexAttribArray(g_a_tile_uv);
    arm64_gl.VertexAttribPointer(g_a_tile_pos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    arm64_gl.VertexAttribPointer(g_a_tile_uv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));

    arm64_gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    arm64_gl.DisableVertexAttribArray(g_a_tile_pos);
    arm64_gl.DisableVertexAttribArray(g_a_tile_uv);
    arm64_gl.DepthMask(GL_TRUE);
    arm64_gl.Disable(GL_BLEND);
}

void arm64_gpu_gles_draw_geo(const float *xyzuv, unsigned nverts,
                             const model2_geo_tri_mat_t *mats, unsigned ntris,
                             const float mvp_matrix[16], int clear_color)
{
    if (!g_ready || !xyzuv || ntris == 0) return;

    upload_sheets();

    /* Clear depth/stencil if palette changed */
    uint32_t psig = model2_palette_state_sig();
    if (psig != g_lut_sig) {
        lut_cache_clear();
        g_lut_sig = psig;
    }

    arm64_gl.Enable(GL_DEPTH_TEST);
    arm64_gl.DepthMask(GL_TRUE);
    arm64_gl.DepthFunc(GL_LEQUAL);
    arm64_gl.Disable(GL_CULL_FACE);

    arm64_gl.UseProgram(g_geo_prog);
    arm64_gl.UniformMatrix4fv(g_u_mvp, 1, GL_FALSE, mvp_matrix);
    arm64_gl.Uniform1i(g_u_sheet, 0);
    arm64_gl.Uniform1i(g_u_lut, 1);

    arm64_gl.BindBuffer(GL_ARRAY_BUFFER, g_geo_vbo);
    arm64_gl.BufferData(GL_ARRAY_BUFFER, nverts * 5 * sizeof(float), xyzuv, GL_STREAM_DRAW);

    arm64_gl.EnableVertexAttribArray(g_a_pos);
    arm64_gl.EnableVertexAttribArray(g_a_uv);
    arm64_gl.VertexAttribPointer(g_a_pos, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    arm64_gl.VertexAttribPointer(g_a_uv, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));

    /* Batch draw triangles by material */
    unsigned t = 0;
    while (t < ntris) {
        const model2_geo_tri_mat_t *m = &mats[t];
        unsigned start_tri = t;
        while (t < ntris && mats[t].colorbase == m->colorbase &&
               mats[t].lumabase == m->lumabase && mats[t].sheet == m->sheet &&
               mats[t].flags == m->flags && mats[t].patch_x == m->patch_x &&
               mats[t].patch_y == m->patch_y) {
            t++;
        }
        unsigned batch_tris = t - start_tri;

        GLuint lut = lut_cache_get(m);
        arm64_gl.ActiveTexture(GL_TEXTURE0);
        arm64_gl.BindTexture(GL_TEXTURE_2D, g_sheet_tex[m->sheet & 1u]);
        arm64_gl.ActiveTexture(GL_TEXTURE1);
        arm64_gl.BindTexture(GL_TEXTURE_2D, lut);

        arm64_gl.Uniform1f(g_u_cutout, (m->flags & MODEL2_GEO_TEX_CUTOUT) ? 1.0f : 0.0f);
        arm64_gl.Uniform4f(g_u_patch, (float)m->patch_x, (float)m->patch_y,
                           (float)(m->patch_w ? m->patch_w : 32),
                           (float)(m->patch_h ? m->patch_h : 32));
        arm64_gl.Uniform2f(g_u_mirror, (m->flags & MODEL2_GEO_TEX_MIRROR_X) ? 1.0f : 0.0f,
                           (m->flags & MODEL2_GEO_TEX_MIRROR_Y) ? 1.0f : 0.0f);

        arm64_gl.DrawArrays(GL_TRIANGLES, start_tri * 3, batch_tris * 3);
    }

    arm64_gl.DisableVertexAttribArray(g_a_pos);
    arm64_gl.DisableVertexAttribArray(g_a_uv);
}
