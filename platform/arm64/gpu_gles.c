#include "gpu_gles.h"
#include "gl_funcs.h"
#include "model2_geo_tex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifndef GL_DEPTH_BITS
#define GL_DEPTH_BITS 0x0D58
#endif
#ifndef GL_STENCIL_BITS
#define GL_STENCIL_BITS 0x0D57
#endif

#define SYS24_W 496
#define SYS24_H 384
#define LUT_CACHE_CAP 1024
#define TILE_SLOTS 4
#define DRAW_SCRATCH_INIT_TRIS 8192

typedef struct {
    uint16_t colorbase;
    uint16_t lumabase;
    uint8_t luma;
    uint8_t cutout;
} lut_entry_t;

static int g_ready = 0;

/* Performance stats + optional frame snapshots (SEGAMOD2_SNAP=<prefix>,
 * SEGAMOD2_SNAP_N=<frames>, default 240) for on-device diagnosis. */
static uint64_t g_frames = 0, g_geo_calls = 0, g_tile_uploads = 0;
static uint64_t g_sheet_reups = 0, g_lut_built = 0, g_lut_wipes = 0;
static uint64_t g_render_ms = 0, g_submit_t0 = 0, g_window_t0 = 0;
static char g_snap_prefix[256];
static unsigned g_snap_n = 240, g_snap_tick = 0;

static uint64_t mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000ull + (uint64_t)t.tv_nsec / 1000000ull;
}

static arm64_aspect_t g_aspect = ASPECT_4_3_CENTERED;
static int g_win_w = 720;
static int g_win_h = 480;

/* Shaders and programs */
static GLuint g_geo_prog = 0;
static GLuint g_solid_prog = 0;
static GLuint g_tile_prog = 0;

/* Uniform locations for Geo Program */
static GLint g_u_mvp = -1;
static GLint g_u_sheet = -1;
static GLint g_u_lut = -1;
static GLint g_u_lut_y = -1;
static GLint g_u_cutout = -1;
static GLint g_u_patch = -1;
static GLint g_u_mirror = -1;

/* Attribute locations for Geo Program */
static GLint g_a_pos = -1;
static GLint g_a_uv = -1;

/* Uniform and attribute locations for Solid Program */
static GLint g_u_solid_mvp = -1;
static GLint g_a_solid_pos = -1;
static GLint g_a_solid_col = -1;

/* Uniform and attribute locations for Tile Program */
static GLint g_u_tile_tex = -1;
static GLint g_a_tile_pos = -1;
static GLint g_a_tile_uv = -1;

/* Textures */
static GLuint g_sheet_tex[2] = {0, 0};
static GLuint g_lut_atlas_tex = 0;
static GLuint g_tile_tex[TILE_SLOTS] = {0, 0, 0, 0};
static int g_tile_tex_init[TILE_SLOTS] = {0, 0, 0, 0};
static uint32_t *g_tile_rgba_buf = NULL;

/* VBO / IBO buffers */
static GLuint g_geo_vbo = 0;
static GLuint g_geo_ibo = 0;
static GLuint g_solid_vbo = 0;
static GLuint g_tile_vbo = 0;

/* Scratch arrays for triangle sorting & indexed drawing */
static unsigned *g_order = NULL;
static uint16_t *g_elem16 = NULL;
static uint8_t *g_solid_rgb = NULL;
static unsigned g_scratch_tris = 0;
static unsigned g_solid_rgb_cap = 0;
static const model2_geo_tri_mat_t *g_sort_mats = NULL;

/* Palette LUT Atlas Cache (16x1024 RGBA) */
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
    if (!vs || !fs) {
        if (vs) arm64_gl.DeleteShader(vs);
        if (fs) arm64_gl.DeleteShader(fs);
        return 0;
    }

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

static unsigned lut_atlas_row(const model2_geo_tri_mat_t *m)
{
    uint8_t cutout = (m->flags & MODEL2_GEO_TEX_CUTOUT) ? 1u : 0u;
    for (unsigned i = 0; i < g_lut_n; i++) {
        if (g_lut_cache[i].colorbase == m->colorbase &&
            g_lut_cache[i].lumabase == m->lumabase &&
            g_lut_cache[i].luma == m->luma &&
            g_lut_cache[i].cutout == cutout) {
            return i;
        }
    }

    uint8_t lut[16 * 4];
    model2_palette_build_texel_lut(m->colorbase, m->lumabase, m->luma, cutout, lut);
    g_lut_built++;

    unsigned row;
    if (g_lut_n < LUT_CACHE_CAP) {
        row = g_lut_n++;
    } else {
        row = g_lut_clock++ % LUT_CACHE_CAP;
    }

    arm64_gl.ActiveTexture(GL_TEXTURE1);
    arm64_gl.BindTexture(GL_TEXTURE_2D, g_lut_atlas_tex);
    arm64_gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    arm64_gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, (GLint)row, 16, 1,
                           GL_RGBA, GL_UNSIGNED_BYTE, lut);

    g_lut_cache[row].colorbase = m->colorbase;
    g_lut_cache[row].lumabase = m->lumabase;
    g_lut_cache[row].luma = m->luma;
    g_lut_cache[row].cutout = cutout;
    return row;
}

static void upload_sheets(void)
{
    uint32_t gen = model2_tex_sheets_dirty_gen();
    if (g_sheets_gen == gen) return;

    uint8_t *r8 = (uint8_t *)malloc(2048u * 1024u);
    if (!r8) return;

    arm64_gl.ActiveTexture(GL_TEXTURE0);
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
        arm64_gl.TexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 2048, 1024, 0,
                            GL_LUMINANCE, GL_UNSIGNED_BYTE, r8);
    }
    free(r8);
    g_sheets_gen = gen;
    g_sheet_reups++;
}

static int ensure_scratch(unsigned ntris, unsigned nverts)
{
    if (ntris > g_scratch_tris) {
        unsigned cap = g_scratch_tris ? g_scratch_tris : DRAW_SCRATCH_INIT_TRIS;
        while (cap < ntris) cap *= 2u;
        unsigned *ord = (unsigned *)realloc(g_order, (size_t)cap * sizeof(unsigned));
        uint16_t *el = (uint16_t *)realloc(g_elem16, (size_t)cap * 3u * sizeof(uint16_t));
        if (!ord || !el) return -1;
        g_order = ord;
        g_elem16 = el;
        g_scratch_tris = cap;
    }
    if (nverts > g_solid_rgb_cap) {
        unsigned vcap = g_solid_rgb_cap ? g_solid_rgb_cap : (DRAW_SCRATCH_INIT_TRIS * 3u);
        while (vcap < nverts) vcap *= 2u;
        uint8_t *rgb = (uint8_t *)realloc(g_solid_rgb, (size_t)vcap * 3u);
        if (!rgb) return -1;
        g_solid_rgb = rgb;
        g_solid_rgb_cap = vcap;
    }
    return 0;
}

/* MAME model2_state::float_to_zval */
static unsigned float_to_zval(float floatval, uint32_t z_adjust)
{
    union {
        float f;
        uint32_t u;
    } conv;
    int32_t fpint;
    int32_t exponent;
    uint32_t mantissa;

    conv.f = floatval;
    fpint = (int32_t)conv.u;
    exponent = ((fpint >> 23) & 0xff) - (int32_t)((z_adjust >> 23) & 0xffu);
    mantissa = (uint32_t)fpint & 0x7fffffu;
    mantissa += 0x400u;
    if (mantissa > 0x7fffffu) {
        exponent++;
        mantissa = (mantissa & 0x7fffffu) >> 1;
    }
    mantissa >>= 11;
    if (fpint < 0) return 0;
    if (exponent < -12) return 0;
    if (exponent < 0) return (mantissa | 0x1000u) >> (unsigned)(-exponent);
    if (exponent < 15) return ((unsigned)(exponent + 1) << 12) | mantissa;
    return 0xffffu;
}

static int mat_order_cmp(const void *a, const void *b)
{
    unsigned ia = *(const unsigned *)a;
    unsigned ib = *(const unsigned *)b;
    const model2_geo_tri_mat_t *ma = &g_sort_mats[ia];
    const model2_geo_tri_mat_t *mb = &g_sort_mats[ib];
    unsigned za = float_to_zval(ma->z_sort, 0u);
    unsigned zb = float_to_zval(mb->z_sort, 0u);
    int ca = (ma->flags & MODEL2_GEO_TEX_CUTOUT) ? 1 : 0;
    int cb = (mb->flags & MODEL2_GEO_TEX_CUTOUT) ? 1 : 0;
    int ta = (ma->flags & MODEL2_GEO_TEX_TEXTURED) ? 1 : 0;
    int tb = (mb->flags & MODEL2_GEO_TEX_TEXTURED) ? 1 : 0;
    int ka = ta ? (ca ? 2 : 1) : 0;
    int kb = tb ? (cb ? 2 : 1) : 0;

    if ((ma->pad & 1u) != (mb->pad & 1u))
        return ((ma->pad & 1u) < (mb->pad & 1u)) ? -1 : 1;
    if (ka != kb)
        return (ka < kb) ? -1 : 1;
    if (za != zb)
        return (za < zb) ? -1 : 1;
    if (za == 0xffffu && ma->z_sort != mb->z_sort)
        return (ma->z_sort < mb->z_sort) ? -1 : 1;
    if (ia != ib)
        return (ia > ib) ? -1 : 1;
    return 0;
}

static int mat_same(const model2_geo_tri_mat_t *a, const model2_geo_tri_mat_t *b)
{
    if (a->colorbase != b->colorbase || a->lumabase != b->lumabase ||
        a->sheet != b->sheet || a->flags != b->flags ||
        a->luma != b->luma || a->pad != b->pad)
        return 0;
    if ((a->flags & MODEL2_GEO_TEX_TEXTURED) == 0)
        return 1;
    return a->patch_x == b->patch_x && a->patch_y == b->patch_y &&
           a->patch_w == b->patch_w && a->patch_h == b->patch_h;
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

    /* Model 2A 3D Geometry GLSL ES 1.00 Shader (highp in FS for 2048x1024 UVs) */
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
        "precision highp float;\n"
        "varying vec2 v_uv;\n"
        "uniform sampler2D uSheet;\n"
        "uniform sampler2D uLut;\n"
        "uniform float uLutY;\n"
        "uniform float uCutout;\n"
        "uniform vec4 uPatch;\n"
        "uniform vec2 uMirror;\n"
        "float wrap_coord(float pix, float size, float mirror) {\n"
        "    float s = max(size, 1.0);\n"
        "    float cell = floor(pix / s);\n"
        "    float local = pix - cell * s;\n"
        "    if (mirror > 0.5 && mod(cell, 2.0) >= 1.0)\n"
        "        local = s - 1.0 - local;\n"
        "    return clamp(floor(local), 0.0, s - 1.0);\n"
        "}\n"
        "void main() {\n"
        "    float u0 = wrap_coord(v_uv.x, uPatch.z, uMirror.x);\n"
        "    float v0 = wrap_coord(v_uv.y, uPatch.w, uMirror.y);\n"
        "    vec2 st = vec2((uPatch.x + u0 + 0.5) / 2048.0,\n"
        "                   (uPatch.y + v0 + 0.5) / 1024.0);\n"
        "    float t = floor(texture2D(uSheet, st).r * 255.0 + 0.5);\n"
        "    vec4 c = texture2D(uLut, vec2((t + 0.5) / 16.0, uLutY));\n"
        "    if (uCutout > 0.5 && c.a < 0.5) discard;\n"
        "    gl_FragColor = vec4(c.rgb, 1.0);\n"
        "}\n";

    g_geo_prog = create_program(geo_vs, geo_fs);
    if (!g_geo_prog) return -1;

    g_u_mvp = arm64_gl.GetUniformLocation(g_geo_prog, "u_mvp");
    g_u_sheet = arm64_gl.GetUniformLocation(g_geo_prog, "uSheet");
    g_u_lut = arm64_gl.GetUniformLocation(g_geo_prog, "uLut");
    g_u_lut_y = arm64_gl.GetUniformLocation(g_geo_prog, "uLutY");
    g_u_cutout = arm64_gl.GetUniformLocation(g_geo_prog, "uCutout");
    g_u_patch = arm64_gl.GetUniformLocation(g_geo_prog, "uPatch");
    g_u_mirror = arm64_gl.GetUniformLocation(g_geo_prog, "uMirror");
    g_a_pos = arm64_gl.GetAttribLocation(g_geo_prog, "a_pos");
    g_a_uv = arm64_gl.GetAttribLocation(g_geo_prog, "a_uv");

    /* Solid untextured polygon shader */
    static const char *solid_vs =
        "precision highp float;\n"
        "attribute vec3 a_pos;\n"
        "attribute vec3 a_col;\n"
        "uniform mat4 u_mvp;\n"
        "varying vec3 v_col;\n"
        "void main() {\n"
        "    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
        "    v_col = a_col;\n"
        "}\n";

    static const char *solid_fs =
        "precision mediump float;\n"
        "varying vec3 v_col;\n"
        "void main() {\n"
        "    gl_FragColor = vec4(v_col, 1.0);\n"
        "}\n";

    g_solid_prog = create_program(solid_vs, solid_fs);
    if (!g_solid_prog) return -1;

    g_u_solid_mvp = arm64_gl.GetUniformLocation(g_solid_prog, "u_mvp");
    g_a_solid_pos = arm64_gl.GetAttribLocation(g_solid_prog, "a_pos");
    g_a_solid_col = arm64_gl.GetAttribLocation(g_solid_prog, "a_col");

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
    arm64_gl.GenTextures(1, &g_lut_atlas_tex);
    arm64_gl.BindTexture(GL_TEXTURE_2D, g_lut_atlas_tex);
    arm64_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    arm64_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    arm64_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    arm64_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    arm64_gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, LUT_CACHE_CAP, 0,
                        GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    arm64_gl.GenTextures(TILE_SLOTS, g_tile_tex);
    for (int i = 0; i < TILE_SLOTS; i++) {
        arm64_gl.BindTexture(GL_TEXTURE_2D, g_tile_tex[i]);
        arm64_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        arm64_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        arm64_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        arm64_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        g_tile_tex_init[i] = 0;
    }

    /* Allocate buffers */
    arm64_gl.GenBuffers(1, &g_geo_vbo);
    arm64_gl.GenBuffers(1, &g_geo_ibo);
    arm64_gl.GenBuffers(1, &g_solid_vbo);
    arm64_gl.GenBuffers(1, &g_tile_vbo);

    static const float tile_quad[16] = {
        -1.0f,  1.0f,  0.0f, 0.0f,
        -1.0f, -1.0f,  0.0f, 1.0f,
         1.0f,  1.0f,  1.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 1.0f
    };
    arm64_gl.BindBuffer(GL_ARRAY_BUFFER, g_tile_vbo);
    arm64_gl.BufferData(GL_ARRAY_BUFFER, sizeof(tile_quad), tile_quad, GL_STATIC_DRAW);

    g_tile_rgba_buf = (uint32_t *)malloc(SYS24_W * SYS24_H * sizeof(uint32_t));

    {
        const char *ver = (const char *)arm64_gl.GetString(GL_VERSION);
        const char *ren = (const char *)arm64_gl.GetString(GL_RENDERER);
        GLint dbits = 0, sbits = 0;
        arm64_gl.GetIntegerv(GL_DEPTH_BITS, &dbits);
        arm64_gl.GetIntegerv(GL_STENCIL_BITS, &sbits);
        printf("gpu_gles: GL=%s RENDERER=%s DEPTH=%d STENCIL=%d (%dx%d)\n",
               ver ? ver : "?", ren ? ren : "?", (int)dbits, (int)sbits, win_w, win_h);
        if (dbits < 24)
            printf("gpu_gles: WARNING shallow depth buffer (%d bits)\n", (int)dbits);
    }
    {
        const char *snap = getenv("SEGAMOD2_SNAP");
        const char *snapn = getenv("SEGAMOD2_SNAP_N");
        if (snap && *snap) {
            snprintf(g_snap_prefix, sizeof(g_snap_prefix), "%s", snap);
            if (snapn) g_snap_n = (unsigned)atoi(snapn);
            if (g_snap_n == 0u) g_snap_n = 1u;
            printf("gpu_gles: snapshots -> %s_*.ppm every %u frames\n",
                   g_snap_prefix, g_snap_n);
        }
    }
    g_ready = 1;
    printf("gpu_gles: initialized Mali-G31 GLES 2.0 pipeline (%dx%d)\n", win_w, win_h);
    return 0;
}

void arm64_gpu_gles_shutdown(void)
{
    if (!g_ready) return;
    if (g_geo_prog) arm64_gl.DeleteProgram(g_geo_prog);
    if (g_solid_prog) arm64_gl.DeleteProgram(g_solid_prog);
    if (g_tile_prog) arm64_gl.DeleteProgram(g_tile_prog);
    if (g_sheet_tex[0]) arm64_gl.DeleteTextures(2, g_sheet_tex);
    if (g_lut_atlas_tex) arm64_gl.DeleteTextures(1, &g_lut_atlas_tex);
    if (g_tile_tex[0]) arm64_gl.DeleteTextures(TILE_SLOTS, g_tile_tex);
    if (g_geo_vbo) arm64_gl.DeleteBuffers(1, &g_geo_vbo);
    if (g_geo_ibo) arm64_gl.DeleteBuffers(1, &g_geo_ibo);
    if (g_solid_vbo) arm64_gl.DeleteBuffers(1, &g_solid_vbo);
    if (g_tile_vbo) arm64_gl.DeleteBuffers(1, &g_tile_vbo);
    free(g_tile_rgba_buf);
    free(g_order);
    free(g_elem16);
    free(g_solid_rgb);
    g_tile_rgba_buf = NULL;
    g_order = NULL;
    g_elem16 = NULL;
    g_solid_rgb = NULL;
    g_scratch_tris = 0;
    g_solid_rgb_cap = 0;
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
    g_submit_t0 = mono_ms();
    g_win_w = display_w;
    g_win_h = display_h;

    int vx, vy, vw, vh;
    calculate_viewport(display_w, display_h, &vx, &vy, &vw, &vh);

    arm64_gl.Viewport(vx, vy, vw, vh);
    arm64_gl.Scissor(vx, vy, vw, vh);
    arm64_gl.Enable(GL_SCISSOR_TEST);
}

static void write_snapshot(void)
{
    char path[320];
    FILE *f;
    unsigned w = (unsigned)g_win_w, h = (unsigned)g_win_h;
    unsigned char *buf = (unsigned char *)malloc((size_t)w * (size_t)h * 4u);
    unsigned y;

    if (!buf || !arm64_gl.ReadPixels) {
        free(buf);
        return;
    }
    if (arm64_gl.GetError) while (arm64_gl.GetError()) {}
    arm64_gl.ReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    snprintf(path, sizeof(path), "%s_%05u.ppm", g_snap_prefix,
             (unsigned)(g_snap_tick / g_snap_n));
    f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (y = h; y-- > 0u;) {
            const unsigned char *row = buf + (size_t)y * (size_t)w * 4u;
            unsigned x;
            for (x = 0; x < w; x++) {
                fputc(row[x * 4u + 0u], f);
                fputc(row[x * 4u + 1u], f);
                fputc(row[x * 4u + 2u], f);
            }
        }
        fclose(f);
        printf("gpu_gles: snapshot %s\n", path);
        fflush(stdout);
    }
    free(buf);
}

void arm64_gpu_gles_end_frame(void)
{
    uint64_t now;

    arm64_gl.Disable(GL_SCISSOR_TEST);

    g_frames++;
    g_render_ms += mono_ms() - g_submit_t0;

    if (g_snap_prefix[0] != '\0') {
        g_snap_tick++;
        if (g_snap_tick % g_snap_n == 0u)
            write_snapshot();
    }

    now = mono_ms();
    if (g_window_t0 == 0u) { g_window_t0 = now; return; }
    if (now - g_window_t0 >= 2000ull) {
        printf("gpu_gles stats: fps=%.1f submit_ms=%.2f draws/s=%llu tiles_up/s=%llu"
               " sheet_reups=%llu lut_entries=%u lut_built=%llu lut_wipes=%llu\n",
               (double)g_frames * 1000.0 / (double)(now - g_window_t0),
               (double)g_render_ms / (double)g_frames,
               (unsigned long long)(g_geo_calls * 1000ull / (now - g_window_t0)),
               (unsigned long long)(g_tile_uploads * 1000ull / (now - g_window_t0)),
               (unsigned long long)g_sheet_reups, g_lut_n,
               (unsigned long long)g_lut_built, (unsigned long long)g_lut_wipes);
        fflush(stdout);
        g_frames = 0; g_render_ms = 0; g_geo_calls = 0; g_tile_uploads = 0;
        g_window_t0 = now;
    }
}

void arm64_gpu_gles_draw_tiles(const uint32_t *src_argb, int transparent)
{
    static const uint32_t *src_last[TILE_SLOTS];
    static uint32_t *src_saved[TILE_SLOTS];
    static int src_valid[TILE_SLOTS];
    int slot = -1, i, same = 0;
    const size_t src_bytes = (size_t)SYS24_W * (size_t)SYS24_H * sizeof(uint32_t);

    if (!g_ready || !src_argb) return;

    /* Dedicated texture per layer slot so g_bitmap and g_bitmap_pri never overwrite each other. */
    for (i = 0; i < TILE_SLOTS; i++)
        if (src_last[i] == src_argb) { slot = i; break; }
    if (slot < 0) {
        for (i = 0; i < TILE_SLOTS; i++)
            if (!src_last[i]) { slot = i; break; }
        if (slot < 0) slot = 0;
        src_last[slot] = src_argb;
        src_valid[slot] = 0;
        if (!src_saved[slot]) src_saved[slot] = (uint32_t *)malloc(src_bytes);
    }

    if (src_valid[slot] && src_saved[slot] &&
        memcmp(src_saved[slot], src_argb, src_bytes) == 0)
        same = 1;

    arm64_gl.ActiveTexture(GL_TEXTURE0);
    arm64_gl.BindTexture(GL_TEXTURE_2D, g_tile_tex[slot]);

    if (!same) {
        uint32_t *dst = g_tile_rgba_buf;
        if (transparent) {
            for (int k = 0; k < SYS24_W * SYS24_H; k++) {
                uint32_t p = src_argb[k];
                uint32_t rb = ((p & 0x00FF0000u) >> 16) | ((p & 0x000000FFu) << 16);
                uint32_t a = (p & 0x00FFFFFFu) ? (p & 0xFF000000u) : 0u;
                dst[k] = a | (p & 0x0000FF00u) | rb;
            }
        } else {
            for (int k = 0; k < SYS24_W * SYS24_H; k++) {
                uint32_t p = src_argb[k];
                dst[k] = (p & 0xFF00FF00u) | ((p & 0x00FF0000u) >> 16) | ((p & 0x000000FFu) << 16);
            }
        }
        if (src_saved[slot]) {
            memcpy(src_saved[slot], src_argb, src_bytes);
            src_valid[slot] = 1;
        }

        if (!g_tile_tex_init[slot]) {
            arm64_gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SYS24_W, SYS24_H, 0,
                                GL_RGBA, GL_UNSIGNED_BYTE, g_tile_rgba_buf);
            g_tile_tex_init[slot] = 1;
        } else {
            arm64_gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SYS24_W, SYS24_H,
                                   GL_RGBA, GL_UNSIGNED_BYTE, g_tile_rgba_buf);
        }
        g_tile_uploads++;
    }

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
    arm64_gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
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
    (void)clear_color;
    if (!g_ready || !xyzuv || !mats || ntris == 0 || nverts < 3u) return;
    if (nverts > 65535u) {
        static int logged_ovf = 0;
        if (!logged_ovf) {
            fprintf(stderr, "gpu_gles: nverts=%u exceeds GL_UNSIGNED_SHORT\n", nverts);
            logged_ovf = 1;
        }
        return;
    }
    if (ensure_scratch(ntris, nverts) != 0) return;

    upload_sheets();

    /* Palette changes invalidate LUT atlas rows without deleting/recreating GL textures */
    uint32_t psig = model2_palette_state_sig();
    if (psig != g_lut_sig) {
        g_lut_n = 0;
        g_lut_clock = 0;
        g_lut_sig = psig;
        g_lut_wipes++;
    }

    for (unsigned t = 0; t < ntris; t++)
        g_order[t] = t;
    g_sort_mats = mats;
    qsort(g_order, ntris, sizeof(unsigned), mat_order_cmp);

    for (unsigned t = 0; t < ntris; t++) {
        uint16_t base = (uint16_t)(g_order[t] * 3u);
        g_elem16[t * 3u + 0u] = base + 0u;
        g_elem16[t * 3u + 1u] = base + 1u;
        g_elem16[t * 3u + 2u] = base + 2u;
    }

    /* Partition sorted order: solids | opaque-tex | cutouts | pad&1 overlays */
    unsigned world_end = 0;
    while (world_end < ntris && !(mats[g_order[world_end]].pad & 1u))
        world_end++;

    unsigned solid_end = 0;
    while (solid_end < world_end &&
           !(mats[g_order[solid_end]].flags & MODEL2_GEO_TEX_TEXTURED))
        solid_end++;

    unsigned opaque_tex_end = solid_end;
    while (opaque_tex_end < world_end &&
           (mats[g_order[opaque_tex_end]].flags & MODEL2_GEO_TEX_TEXTURED) &&
           !(mats[g_order[opaque_tex_end]].flags & MODEL2_GEO_TEX_CUTOUT))
        opaque_tex_end++;

    /* Populate per-vertex RGB only if there are solid triangles */
    int has_solids = (solid_end > 0);
    if (!has_solids) {
        for (unsigned t = world_end; t < ntris; t++) {
            if (!(mats[g_order[t]].flags & MODEL2_GEO_TEX_TEXTURED)) {
                has_solids = 1;
                break;
            }
        }
    }
    if (has_solids) {
        uint8_t rgb[3] = {0, 0, 0};
        uint16_t last_cb = 0xffffu, last_lb = 0xffffu;
        uint8_t last_luma = 0xffu;
        int have_rgb = 0;
        for (unsigned t = 0; t < ntris; t++) {
            unsigned src = g_order[t];
            if (mats[src].flags & MODEL2_GEO_TEX_TEXTURED)
                continue;
            if (!have_rgb || mats[src].colorbase != last_cb ||
                mats[src].lumabase != last_lb || mats[src].luma != last_luma) {
                model2_palette_lookup_solid(mats[src].colorbase, mats[src].luma, rgb);
                last_cb = mats[src].colorbase;
                last_lb = mats[src].lumabase;
                last_luma = mats[src].luma;
                have_rgb = 1;
            }
            unsigned base = src * 3u;
            for (unsigned k = 0; k < 3u; k++) {
                g_solid_rgb[(base + k) * 3u + 0u] = rgb[0];
                g_solid_rgb[(base + k) * 3u + 1u] = rgb[1];
                g_solid_rgb[(base + k) * 3u + 2u] = rgb[2];
            }
        }
        arm64_gl.BindBuffer(GL_ARRAY_BUFFER, g_solid_vbo);
        arm64_gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nverts * 3u), g_solid_rgb, GL_STREAM_DRAW);
    }

    arm64_gl.BindBuffer(GL_ARRAY_BUFFER, g_geo_vbo);
    arm64_gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nverts * 5u * sizeof(float)), xyzuv, GL_STREAM_DRAW);
    arm64_gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_geo_ibo);
    arm64_gl.BufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(ntris * 3u * sizeof(uint16_t)), g_elem16, GL_STREAM_DRAW);

    arm64_gl.Disable(GL_BLEND);
    arm64_gl.Disable(GL_CULL_FACE);
    if (arm64_gl.ClearStencil) arm64_gl.ClearStencil(0);
    arm64_gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    arm64_gl.StencilMask(0xffu);

    /* State trackers for textured shader to avoid redundant GL calls */
    int tex_bound_prog = 0;
    int cur_sheet = -1;
    int cur_cutout = -1;
    float cur_lut_y = -1.0f;
    uint16_t cur_px = 0xffffu, cur_py = 0xffffu, cur_pw = 0xffffu, cur_ph = 0xffffu;
    int cur_mx = -1, cur_my = -1;

    #define BIND_SOLID_PROG() do { \
        if (tex_bound_prog) { \
            arm64_gl.DisableVertexAttribArray(g_a_pos); \
            arm64_gl.DisableVertexAttribArray(g_a_uv); \
        } \
        arm64_gl.UseProgram(g_solid_prog); \
        arm64_gl.UniformMatrix4fv(g_u_solid_mvp, 1, GL_FALSE, mvp_matrix); \
        arm64_gl.BindBuffer(GL_ARRAY_BUFFER, g_geo_vbo); \
        arm64_gl.EnableVertexAttribArray(g_a_solid_pos); \
        arm64_gl.VertexAttribPointer(g_a_solid_pos, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0); \
        arm64_gl.BindBuffer(GL_ARRAY_BUFFER, g_solid_vbo); \
        arm64_gl.EnableVertexAttribArray(g_a_solid_col); \
        arm64_gl.VertexAttribPointer(g_a_solid_col, 3, GL_UNSIGNED_BYTE, GL_TRUE, 3, (void *)0); \
        tex_bound_prog = 0; \
    } while (0)

    #define BIND_TEX_PROG() do { \
        if (!tex_bound_prog) { \
            arm64_gl.DisableVertexAttribArray(g_a_solid_pos); \
            arm64_gl.DisableVertexAttribArray(g_a_solid_col); \
            arm64_gl.UseProgram(g_geo_prog); \
            arm64_gl.UniformMatrix4fv(g_u_mvp, 1, GL_FALSE, mvp_matrix); \
            arm64_gl.Uniform1i(g_u_sheet, 0); \
            arm64_gl.Uniform1i(g_u_lut, 1); \
            arm64_gl.ActiveTexture(GL_TEXTURE1); \
            arm64_gl.BindTexture(GL_TEXTURE_2D, g_lut_atlas_tex); \
            arm64_gl.BindBuffer(GL_ARRAY_BUFFER, g_geo_vbo); \
            arm64_gl.EnableVertexAttribArray(g_a_pos); \
            arm64_gl.EnableVertexAttribArray(g_a_uv); \
            arm64_gl.VertexAttribPointer(g_a_pos, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0); \
            arm64_gl.VertexAttribPointer(g_a_uv, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float))); \
            tex_bound_prog = 1; \
            cur_sheet = -1; cur_cutout = -1; cur_lut_y = -1.0f; \
            cur_px = 0xffffu; cur_mx = -1; cur_my = -1; \
        } \
    } while (0)

    #define DRAW_TEX_BATCH(start_idx, batch_tris, mptr) do { \
        unsigned row = lut_atlas_row(mptr); \
        float lut_y = ((float)row + 0.5f) / (float)LUT_CACHE_CAP; \
        int sh = (int)((mptr)->sheet & 1u); \
        int cut = ((mptr)->flags & MODEL2_GEO_TEX_CUTOUT) ? 1 : 0; \
        uint16_t pw = (mptr)->patch_w ? (mptr)->patch_w : 32u; \
        uint16_t ph = (mptr)->patch_h ? (mptr)->patch_h : 32u; \
        int mx = ((mptr)->flags & MODEL2_GEO_TEX_MIRROR_X) ? 1 : 0; \
        int my = ((mptr)->flags & MODEL2_GEO_TEX_MIRROR_Y) ? 1 : 0; \
        if (sh != cur_sheet) { \
            arm64_gl.ActiveTexture(GL_TEXTURE0); \
            arm64_gl.BindTexture(GL_TEXTURE_2D, g_sheet_tex[sh]); \
            cur_sheet = sh; \
        } \
        if (lut_y != cur_lut_y) { \
            arm64_gl.Uniform1f(g_u_lut_y, lut_y); \
            cur_lut_y = lut_y; \
        } \
        if (cut != cur_cutout) { \
            arm64_gl.Uniform1f(g_u_cutout, cut ? 1.0f : 0.0f); \
            cur_cutout = cut; \
        } \
        if ((mptr)->patch_x != cur_px || (mptr)->patch_y != cur_py || \
            pw != cur_pw || ph != cur_ph) { \
            arm64_gl.Uniform4f(g_u_patch, (float)(mptr)->patch_x, (float)(mptr)->patch_y, \
                               (float)pw, (float)ph); \
            cur_px = (mptr)->patch_x; cur_py = (mptr)->patch_y; \
            cur_pw = pw; cur_ph = ph; \
        } \
        if (mx != cur_mx || my != cur_my) { \
            arm64_gl.Uniform2f(g_u_mirror, mx ? 1.0f : 0.0f, my ? 1.0f : 0.0f); \
            cur_mx = mx; cur_my = my; \
        } \
        arm64_gl.DrawElements(GL_TRIANGLES, (GLsizei)((batch_tris) * 3u), GL_UNSIGNED_SHORT, \
                              (void *)(uintptr_t)((start_idx) * 3u * sizeof(uint16_t))); \
        g_geo_calls++; \
    } while (0)

    /* --- Pass A: solids --- */
    if (solid_end > 0) {
        arm64_gl.Disable(GL_STENCIL_TEST);
        arm64_gl.Enable(GL_DEPTH_TEST);
        arm64_gl.DepthMask(GL_TRUE);
        arm64_gl.DepthFunc(GL_LESS);
        BIND_SOLID_PROG();
        arm64_gl.DrawElements(GL_TRIANGLES, (GLsizei)(solid_end * 3u), GL_UNSIGNED_SHORT, (void *)0);
        g_geo_calls++;
        arm64_gl.DisableVertexAttribArray(g_a_solid_pos);
        arm64_gl.DisableVertexAttribArray(g_a_solid_col);
    }

    /* --- Pass B: opaque-tex fillmap + depth write --- */
    if (opaque_tex_end > solid_end) {
        arm64_gl.Enable(GL_DEPTH_TEST);
        arm64_gl.DepthMask(GL_TRUE);
        arm64_gl.DepthFunc(GL_LEQUAL);
        arm64_gl.Enable(GL_STENCIL_TEST);
        arm64_gl.StencilFunc(GL_EQUAL, 0, 0xff);
        arm64_gl.StencilOp(GL_KEEP, GL_KEEP, GL_INCR);
        BIND_TEX_PROG();

        unsigned run_start = solid_end;
        for (unsigned t = solid_end + 1u; t <= opaque_tex_end; t++) {
            int same = (t < opaque_tex_end) &&
                       mat_same(&mats[g_order[run_start]], &mats[g_order[t]]);
            if (same) continue;
            DRAW_TEX_BATCH(run_start, t - run_start, &mats[g_order[run_start]]);
            run_start = t;
        }
    }

    /* --- Pass C: cutouts on fresh fillmap, depth test vs A/B --- */
    if (world_end > opaque_tex_end) {
        arm64_gl.Clear(GL_STENCIL_BUFFER_BIT);
        arm64_gl.Enable(GL_DEPTH_TEST);
        arm64_gl.DepthMask(GL_FALSE);
        arm64_gl.DepthFunc(GL_LEQUAL);
        arm64_gl.Enable(GL_STENCIL_TEST);
        arm64_gl.StencilFunc(GL_EQUAL, 0, 0xff);
        arm64_gl.StencilOp(GL_KEEP, GL_KEEP, GL_INCR);
        BIND_TEX_PROG();

        unsigned run_start = opaque_tex_end;
        for (unsigned t = opaque_tex_end + 1u; t <= world_end; t++) {
            int same = (t < world_end) &&
                       mat_same(&mats[g_order[run_start]], &mats[g_order[t]]);
            if (same) continue;
            DRAW_TEX_BATCH(run_start, t - run_start, &mats[g_order[run_start]]);
            run_start = t;
        }
    }

    /* --- Pass D: pad&1 HUD overlays after world --- */
    if (ntris > world_end) {
        arm64_gl.Disable(GL_STENCIL_TEST);
        arm64_gl.Disable(GL_DEPTH_TEST);
        arm64_gl.DepthMask(GL_FALSE);
        unsigned run_start = world_end;
        for (unsigned t = world_end + 1u; t <= ntris; t++) {
            int same = (t < ntris) &&
                       mat_same(&mats[g_order[run_start]], &mats[g_order[t]]);
            if (same) continue;
            const model2_geo_tri_mat_t *batch = &mats[g_order[run_start]];
            if (batch->flags & MODEL2_GEO_TEX_TEXTURED) {
                BIND_TEX_PROG();
                DRAW_TEX_BATCH(run_start, t - run_start, batch);
            } else {
                BIND_SOLID_PROG();
                arm64_gl.DrawElements(GL_TRIANGLES, (GLsizei)((t - run_start) * 3u), GL_UNSIGNED_SHORT,
                                      (void *)(uintptr_t)(run_start * 3u * sizeof(uint16_t)));
                g_geo_calls++;
            }
            run_start = t;
        }
    }

    if (tex_bound_prog) {
        arm64_gl.DisableVertexAttribArray(g_a_pos);
        arm64_gl.DisableVertexAttribArray(g_a_uv);
    } else if (has_solids) {
        arm64_gl.DisableVertexAttribArray(g_a_solid_pos);
        arm64_gl.DisableVertexAttribArray(g_a_solid_col);
    }

    #undef BIND_SOLID_PROG
    #undef BIND_TEX_PROG
    #undef DRAW_TEX_BATCH

    arm64_gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    arm64_gl.Disable(GL_STENCIL_TEST);
    arm64_gl.Enable(GL_DEPTH_TEST);
    arm64_gl.DepthMask(GL_TRUE);
    arm64_gl.DepthFunc(GL_LEQUAL);
}
