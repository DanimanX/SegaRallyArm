/* Native OpenGL ES 2.0 / GLSL ES 1.00 hardware renderer for Sega Rally ARM64.
 * Tuned for Mali-G31 MP2 (Anbernic RG34XX, RG35XX+, Powkiddy, TrimUI).
 */
#ifndef PLATFORM_ARM64_GPU_GLES_H
#define PLATFORM_ARM64_GPU_GLES_H

#include <stdint.h>
#include "gl_funcs.h"
#include "model2_geo.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum arm64_aspect {
    ASPECT_4_3_CENTERED = 0, /* Crisp 4:3 arcade aspect (620x480 on RG34XX) */
    ASPECT_FULLSCREEN = 1    /* Stretch to fill panel (720x480 3:2) */
} arm64_aspect_t;

int arm64_gpu_gles_init(int win_w, int win_h);
void arm64_gpu_gles_shutdown(void);

void arm64_gpu_gles_set_aspect(arm64_aspect_t mode);
arm64_aspect_t arm64_gpu_gles_get_aspect(void);

void arm64_gpu_gles_begin_frame(int display_w, int display_h);
void arm64_gpu_gles_end_frame(void);
void arm64_gpu_gles_note_swap(uint64_t ms);

/* Upload and render System 24 2D Tilemap overlay */
void arm64_gpu_gles_draw_tiles(const uint32_t *src_argb, int transparent);

/* Render 3D Model 2A geometry with projection matrix and textures */
void arm64_gpu_gles_draw_geo(const float *xyzuv, unsigned nverts,
                             const model2_geo_tri_mat_t *mats, unsigned ntris,
                             const float mvp_matrix[16], int clear_color);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_ARM64_GPU_GLES_H */
