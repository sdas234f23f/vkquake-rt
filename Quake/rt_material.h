// Q2RTX-style material definitions (phase 4.5).
// Ported from Q2RTX material.c and adapted to the vkquake host: materials are
// loaded from materials/*.yaml files (global + <map>.yaml) found either on disk
// or inside a mounted .pkz archive, and are used to synthesize the vkpt
// RGBA8 material textures (albedo-alpha, roughness-metallic-emissive, normal).

#ifndef RT_MATERIAL_H
#define RT_MATERIAL_H

#include "quakedef.h"

enum {
    RT_MAT_KIND_INVALID    = 0,
    RT_MAT_KIND_REGULAR    = 1,
    RT_MAT_KIND_CHROME     = 2,
    RT_MAT_KIND_WATER      = 3,
    RT_MAT_KIND_LAVA       = 4,
    RT_MAT_KIND_SLIME      = 5,
    RT_MAT_KIND_GLASS      = 6,
    RT_MAT_KIND_SKY        = 7,
    RT_MAT_KIND_INVISIBLE  = 8,
    RT_MAT_KIND_SCREEN     = 9,
    RT_MAT_KIND_CAMERA     = 10,
};

typedef struct rt_material_s {
    char name[MAX_QPATH];
    char filename_base[MAX_QPATH];
    char filename_normals[MAX_QPATH];
    char filename_emissive[MAX_QPATH];
    char filename_mask[MAX_QPATH];
    char filename_gloss[MAX_QPATH];
    float bump_scale;
    float roughness_override;
    float metalness_factor;
    float emissive_factor;
    float specular_factor;
    float base_factor;
    int kind;
    qboolean is_light;
    qboolean light_styles;
    qboolean has_metalness_factor;
    qboolean metalness_from_normal_alpha;
    qboolean bsp_radiance;
    float default_radiance;
    vec3_t color_emissive;
    qboolean has_color_emissive;
    float color_emissive_threshold;
    vec3_t light_color;
    qboolean has_light_color;
    float light_brightness;
    float light_upoffset;
    qboolean mirror;
    qboolean exact_normals;
    qboolean force_rasterize;
    qboolean valid;
} rt_material_t;

void RT_MAT_Init(void);
void RT_MAT_Shutdown(void);

void RT_MAT_ChangeMap(const char *mapname);

rt_material_t *RT_MAT_Find(const char *name);

enum {
    RT_MAT_TEX_BASE,
    RT_MAT_TEX_NORMALS,
    RT_MAT_TEX_EMISSIVE,
    RT_MAT_TEX_MASK,
    RT_MAT_TEX_GLOSS,
};
byte *RT_MAT_LoadTexture(const rt_material_t *mat, int which, int *outWidth, int *outHeight);

qboolean RT_MAT_Enabled(void);

void RT_MAT_Cmd(void);

#endif
