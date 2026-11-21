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

/* Performance stats + optional frame snapshots (SEGAMOD2_SNAP=<prefix>,
 * SEGAMOD2_SNAP_N=<frames>, default 240) for on-device diagnosis. */
static uint64_t g_frames = 0, g_geo_calls = 0, g_tile_uploads = 0;
static uint64_t g_sheet_reups = 0, g_lut_built = 0, g_lut_wipes = 0;
static uint64_t g_win_ms = 0;
static struct timespec g_stat_t;
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
    clock_gettime(CLOCK_MONOTONIC, &g_stat_t);
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

static uint64_t g_submit_t0 = 0, g_render_ms = 0;
static uint64_t g_window_t0 = 0;

void arm64_gpu_gles_begin_frame(int display_w, int display_h)
{
    g_submit_t0 = mono_ms();
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

static void write_snapshot(void)
{
    char path[320];
    FILE *f;
    unsigned w = (unsigned)g_win_w, h = (unsigned)g_win_h;
    unsigned char *buf = (unsigned char *)malloc((size_t)w * (size_t)h * 4u);
    unsigned y;

    if (!buf || !arm64_gl.ReadPixels) return;
    if (arm64_gl.GetError) while (arm64_gl.GetError()) {}
    arm64_gl.ReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    snprintf(path, sizeof(path), "%s_%05u.ppm", g_snap_prefix,
             (unsigned)(g_snap_tick / g_snap_n));
    f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (y = h; y-- > 0u;) {           /* GL rows bottom-up -> PPM top-down */
            const unsigned char *row = buf + (size_t)y * (size_t)w * 4u;
            unsigned x;
            for (x = 0; x < w; x++)
                fputc(row[x * 4u + 0u], f), fputc(row[x * 4u + 1u], f),
                      fputc(row[x * 4u + 2u], f);
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
               " sheet_reups=%llu lut_entries=%d lut_built=%llu lut_wipes=%llu\n",
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
    static const uint32_t *src_last[4];
    static uint32_t *src_saved[4];
    static int src_valid[4];
    int slot = -1, i, same = 0;
    const size_t src_bytes = (size_t)SYS24_W * (size_t)SYS24_H * sizeof(uint32_t);

    if (!g_ready || !src_argb) return;

    /* Stable per-layer slots (primary, pri, ...): skip convert+upload when the
     * Sys24 framebuffer content is identical to the previous frame. */
    for (i = 0; i < 4; i++)
        if (src_last[i] == src_argb) { slot = i; break; }
    if (slot < 0) {
        for (i = 0; i < 4; i++)
            if (!src_last[i]) { slot = i; break; }
        if (slot < 0) slot = 0;
        src_last[slot] = src_argb;
        src_valid[slot] = 0;
        if (!src_saved[slot]) src_saved[slot] = (uint32_t *)malloc(src_bytes);
    }

    if (src_valid[slot] && src_saved[slot] &&
        memcmp(src_saved[slot], src_argb, src_bytes) == 0)
        same = 1;

    if (!same) {
        /* Convert ARGB to RGBA with alpha punch for transparency */
        for (int k = 0; k < SYS24_W * SYS24_H; k++) {
            uint32_t p = src_argb[k];
            uint8_t a = (p >> 24) & 0xFF;
            uint8_t r = (p >> 16) & 0xFF;
            uint8_t g = (p >> 8) & 0xFF;
            uint8_t b = p & 0xFF;
            if (transparent && (p & 0x00FFFFFF) == 0) {
                a = 0;
            }
            g_tile_rgba_buf[k] = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
        }
        if (src_saved[slot]) {
            memcpy(src_saved[slot], src_argb, src_bytes);
            src_valid[slot] = 1;
        }

        arm64_gl.ActiveTexture(GL_TEXTURE0);
        arm64_gl.BindTexture(GL_TEXTURE_2D, g_tile_tex);
        arm64_gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SYS24_W, SYS24_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_tile_rgba_buf);
        g_tile_uploads++;
    }

    /* geo batching leaves TEXTURE0 bound to a sheet; re-bind the tile tex. */
    arm64_gl.ActiveTexture(GL_TEXTURE0);
    arm64_gl.BindTexture(GL_TEXTURE_2D, g_tile_tex);

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
        g_lut_wipes++;
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
        g_geo_calls++;
    }

    arm64_gl.DisableVertexAttribArray(g_a_pos);
    arm64_gl.DisableVertexAttribArray(g_a_uv);
}
