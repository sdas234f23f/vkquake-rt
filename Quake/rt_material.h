// Q2RTX-style .mat material definitions (phase 4.5).
// Ported from Q2RTX material.c and adapted to the vkquake host: materials are
// loaded from materials/*.mat files (global + <map>.mat) found either on disk
// or inside a mounted .pkz archive, and are used to synthesize the RTGL1
// RGBA8 material textures (albedo-alpha, roughness-metallic-emissive, normal).

#ifndef RT_MATERIAL_H
#define RT_MATERIAL_H

#include "quakedef.h"

// material kinds (Q2RTX materialKinds[]), used later by the reflect/refract
// and sky paths once the material ids flow into the renderer
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
    char name[MAX_QPATH];              // lowercase texture name, no extension
    char filename_base[MAX_QPATH];     // base texture path (without .tga)
    char filename_normals[MAX_QPATH];
    char filename_emissive[MAX_QPATH];
    char filename_mask[MAX_QPATH];
    char filename_gloss[MAX_QPATH];    // gloss map (HD packs: _gloss) -> roughness = 1 - gloss
    float bump_scale;
    float roughness_override;          // 0 = unset
    float metalness_factor;
    float emissive_factor;
    float specular_factor;
    float base_factor;
    int kind;
    qboolean is_light;
    qboolean light_styles;
    qboolean bsp_radiance;
    float default_radiance;
    qboolean synth_emissive;
    int emissive_threshold;
    qboolean valid;
} rt_material_t;

// Clears the table and loads materials/*.mat (global materials) + any <map>.mat
// for the current map (see RT_MAT_ChangeMap).
void RT_MAT_Init(void);
void RT_MAT_Shutdown(void);

// Loads <mapname>.mat (map-specific materials) on top of the global ones.
void RT_MAT_ChangeMap(const char *mapname);

// Finds a material by texture name (no extension, case-insensitive).
// Returns NULL if no material is defined for this texture.
rt_material_t *RT_MAT_Find(const char *name);

// Auto-detects a material from the HD texture pack naming convention when
// no .mat definition exists: it looks for <name>_norm (normal map),
// <name>_gloss (gloss -> roughness), <name>_luma / <name>_glow (emissive) and
// <name>_bump (legacy bump, used only if _norm is missing). Fills *out and
// returns true if at least one suffix texture was found.
qboolean RT_MAT_AutoDetect(const char *name, rt_material_t *out);

// Loads the given material texture (RT_MAT_TEX_*) as RGBA8 from a .pkz
// archive or the filesystem (TGA type 2/10, 24/32-bit). Returns a heap
// buffer (free with Mem_Free) or NULL.
enum {
    RT_MAT_TEX_BASE,
    RT_MAT_TEX_NORMALS,
    RT_MAT_TEX_EMISSIVE,
    RT_MAT_TEX_MASK,
    RT_MAT_TEX_GLOSS,
};
byte *RT_MAT_LoadTexture(const rt_material_t *mat, int which, int *outWidth, int *outHeight);

// True if the material system is enabled (rt_materials cvar).
qboolean RT_MAT_Enabled(void);

// Console command "rt_mat [name]": prints the material table or one material.
void RT_MAT_Cmd(void);

#endif // RT_MATERIAL_H
