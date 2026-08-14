// Q2RTX-style .mat material definitions (phase 4.5).
// Ported from Q2RTX material.c (GPL v2) and adapted to the vkquake host.

#include "quakedef.h"
#include "rt_material.h"
#include "rt_pkz.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4505)
#endif

// STB_IMAGE config (phase 4.5: JPG/PNG decode for HD texture packs):
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_LINEAR
#define STBI_NO_STDIO
#define STBI_MALLOC(sz)		Mem_Alloc (sz)
#define STBI_REALLOC(p, newsz) Mem_Realloc (p, newsz)
#define STBI_FREE(p)		Mem_Free (p)
#include "stb_image.h"

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define RT_MAT_MAX_MATERIALS 2048
#define RT_MAT_MAX_GLOBAL   4096
#define RT_MAT_MAX_MAP      1024

static rt_material_t rt_global_materials[RT_MAT_MAX_GLOBAL];
static int rt_global_count = 0;
static rt_material_t rt_map_materials[RT_MAT_MAX_MAP];
static int rt_map_count = 0;
static qboolean rt_initialized = false;
static cmd_function_t *rt_mat_cmd = NULL;

static cvar_t rt_materials = { "rt_materials", "1", CVAR_ARCHIVE };
cvar_t rt_mat_debug = { "rt_mat_debug", "0", 0 };

// ---------------------------------------------------------------------------
// tiny TGA decoder (type 2 uncompressed / type 10 RLE, 24/32-bit) from memory
// ---------------------------------------------------------------------------

static byte *rt_load_file(const char *name, int *outLen)
{
    // .pkz archives first, then the filesystem (dirs + .pak)
    byte *b = RT_PKZ_LoadFile(name, outLen);
    if (b)
    {
        return b;
    }

    unsigned int path_id;
    byte *fs = COM_LoadFile(name, &path_id);
    if (fs)
    {
        if (outLen)
        {
            *outLen = com_filesize;
        }
        return fs;
    }

    if (outLen)
    {
        *outLen = 0;
    }
    return NULL;
}

static void rt_load_file_free(byte *data)
{
    Mem_Free(data);
}

static byte *rt_decode_tga(const byte *buf, int len, int *outW, int *outH)
{
    if (len < 18)
    {
        return NULL;
    }

    const byte id_length = buf[0];
    const byte colormap_type = buf[1];
    const byte image_type = buf[2];
    const int width = buf[12] | (buf[13] << 8);
    const int height = buf[14] | (buf[15] << 8);
    const byte pixel_size = buf[16];
    const byte attributes = buf[17];

    if (colormap_type != 0)
    {
        return NULL; // paletted TGAs are not handled
    }
    if (image_type != 2 && image_type != 10)
    {
        return NULL; // only true-color and RLE true-color
    }
    if (pixel_size != 24 && pixel_size != 32)
    {
        return NULL;
    }
    if (width <= 0 || height <= 0)
    {
        return NULL;
    }

    const qboolean upside_down = !(attributes & 0x20);
    const int numPixels = width * height;
    byte *out = (byte *)Mem_Alloc(numPixels * 4);
    if (!out)
    {
        return NULL;
    }

    int pos = 18 + id_length;
    const int bpp = pixel_size / 8;

    for (int row = height - 1; row >= 0; row--)
    {
        const int realrow = upside_down ? row : height - 1 - row;
        byte *pixbuf = out + realrow * width * 4;

        for (int column = 0; column < width; column++)
        {
            if (image_type == 10) // RLE
            {
                if (pos >= len)
                {
                    Mem_Free(out);
                    return NULL;
                }
                const byte packet = buf[pos++];
                const int count = (packet & 0x7f) + 1;
                if (packet & 0x80) // run-length packet
                {
                    if (pos + bpp > len || column + count > width)
                    {
                        Mem_Free(out);
                        return NULL;
                    }
                    const byte r = buf[pos + 2];
                    const byte g = buf[pos + 1];
                    const byte b = buf[pos + 0];
                    const byte a = (bpp == 4) ? buf[pos + 3] : 255;
                    pos += bpp;
                    for (int k = 0; k < count; k++)
                    {
                        *pixbuf++ = r; *pixbuf++ = g; *pixbuf++ = b; *pixbuf++ = a;
                    }
                    column += count - 1;
                }
                else // raw packet
                {
                    for (int k = 0; k < count; k++)
                    {
                        if (pos + bpp > len || column + k >= width)
                        {
                            Mem_Free(out);
                            return NULL;
                        }
                        *pixbuf++ = buf[pos + 2];
                        *pixbuf++ = buf[pos + 1];
                        *pixbuf++ = buf[pos + 0];
                        *pixbuf++ = (bpp == 4) ? buf[pos + 3] : 255;
                        pos += bpp;
                    }
                    column += count - 1;
                }
            }
            else // type 2
            {
                if (pos + bpp > len)
                {
                    Mem_Free(out);
                    return NULL;
                }
                *pixbuf++ = buf[pos + 2];
                *pixbuf++ = buf[pos + 1];
                *pixbuf++ = buf[pos + 0];
                *pixbuf++ = (bpp == 4) ? buf[pos + 3] : 255;
                pos += bpp;
            }
        }
    }

    if (outW) *outW = width;
    if (outH) *outH = height;
    return out;
}

byte *RT_MAT_LoadTexture(const rt_material_t *mat, int which, int *outWidth, int *outHeight)
{
    const char *base = NULL;
    switch (which)
    {
        case RT_MAT_TEX_BASE:     base = mat->filename_base; break;
        case RT_MAT_TEX_NORMALS:  base = mat->filename_normals; break;
        case RT_MAT_TEX_EMISSIVE: base = mat->filename_emissive; break;
        case RT_MAT_TEX_MASK:     base = mat->filename_mask; break;
        case RT_MAT_TEX_GLOSS:    base = mat->filename_gloss; break;
        default: return NULL;
    }
    if (!base[0])
    {
        return NULL;
    }

    // .mat entries (Q2RTX style) store the full file name WITH extension
    // (e.g. textures/e1u1/foo_n.tga), while auto-detected names have none.
    const char *dot = strrchr(base, '.');
    const qboolean hasExt = (dot != NULL && dot[1] != '\0');

    if (hasExt)
    {
        int len = 0;
        byte *file = rt_load_file(base, &len);
        if (!file)
        {
            if (outWidth) *outWidth = 0;
            if (outHeight) *outHeight = 0;
            return NULL;
        }
        byte *decoded = NULL;
        if (!q_strcasecmp(dot, ".tga"))
        {
            decoded = rt_decode_tga(file, len, outWidth, outHeight);
        }
        else
        {
            int w = 0, h = 0, c = 0;
            decoded = stbi_load_from_memory(file, len, &w, &h, &c, 4);
            if (decoded)
            {
                if (outWidth) *outWidth = w;
                if (outHeight) *outHeight = h;
            }
        }
        rt_load_file_free(file);
        if (!decoded)
        {
            if (outWidth) *outWidth = 0;
            if (outHeight) *outHeight = 0;
        }
        return decoded;
    }

    // try .tga (own decoder), then .jpg/.png (stb_image)
    static const char *exts[] = { "tga", "jpg", "png" };
    for (int e = 0; e < 3; e++)
    {
        char name[MAX_QPATH];
        q_snprintf(name, sizeof(name), "%s.%s", base, exts[e]);

        int len = 0;
        byte *file = rt_load_file(name, &len);
        if (!file)
        {
            continue;
        }

        if (e == 0) // tga
        {
            byte *decoded = rt_decode_tga(file, len, outWidth, outHeight);
            rt_load_file_free(file);
            if (decoded)
            {
                return decoded;
            }
        }
        else // jpg / png
        {
            int w = 0, h = 0, comp = 0;
            byte *decoded = stbi_load_from_memory(file, len, &w, &h, &comp, 4);
            rt_load_file_free(file);
            if (decoded)
            {
                if (outWidth) *outWidth = w;
                if (outHeight) *outHeight = h;
                return decoded;
            }
        }
    }

    if (outWidth) *outWidth = 0;
    if (outHeight) *outHeight = 0;
    return NULL;
}

// ---------------------------------------------------------------------------
// .mat parser (ported from Q2RTX material.c load_material_file)
// ---------------------------------------------------------------------------

static void rt_mat_reset(rt_material_t *mat)
{
    memset(mat, 0, sizeof(*mat));
    mat->bump_scale = 1.0f;
    mat->metalness_factor = 0.0f; // JPG normals carry no alpha -> keep non-metallic unless a .mat says otherwise
    mat->emissive_factor = 1.0f;
    mat->specular_factor = 1.0f;
    mat->base_factor = 1.0f;
    mat->kind = RT_MAT_KIND_REGULAR;
}

static int rt_mat_parse_kind(const char *kindname)
{
    if (!q_strcasecmp(kindname, "REGULAR"))   return RT_MAT_KIND_REGULAR;
    if (!q_strcasecmp(kindname, "CHROME"))    return RT_MAT_KIND_CHROME;
    if (!q_strcasecmp(kindname, "WATER"))     return RT_MAT_KIND_WATER;
    if (!q_strcasecmp(kindname, "LAVA"))      return RT_MAT_KIND_LAVA;
    if (!q_strcasecmp(kindname, "SLIME"))     return RT_MAT_KIND_SLIME;
    if (!q_strcasecmp(kindname, "GLASS"))     return RT_MAT_KIND_GLASS;
    if (!q_strcasecmp(kindname, "SKY"))       return RT_MAT_KIND_SKY;
    if (!q_strcasecmp(kindname, "INVISIBLE")) return RT_MAT_KIND_INVISIBLE;
    if (!q_strcasecmp(kindname, "SCREEN"))    return RT_MAT_KIND_SCREEN;
    if (!q_strcasecmp(kindname, "CAMERA"))    return RT_MAT_KIND_CAMERA;
    return RT_MAT_KIND_REGULAR;
}

static void rt_mat_set_attribute(rt_material_t *mat, const char *key, const char *value)
{
    if (!q_strcasecmp(key, "bump_scale"))
        mat->bump_scale = (float)atof(value);
    else if (!q_strcasecmp(key, "roughness_override"))
        mat->roughness_override = (float)atof(value);
    else if (!q_strcasecmp(key, "metalness_factor"))
        mat->metalness_factor = (float)atof(value);
    else if (!q_strcasecmp(key, "emissive_factor"))
        mat->emissive_factor = (float)atof(value);
    else if (!q_strcasecmp(key, "specular_factor"))
        mat->specular_factor = (float)atof(value);
    else if (!q_strcasecmp(key, "base_factor"))
        mat->base_factor = (float)atof(value);
    else if (!q_strcasecmp(key, "kind"))
        mat->kind = rt_mat_parse_kind(value);
    else if (!q_strcasecmp(key, "is_light"))
        mat->is_light = (value[0] != '0');
    else if (!q_strcasecmp(key, "light_styles"))
        mat->light_styles = (value[0] != '0');
    else if (!q_strcasecmp(key, "bsp_radiance"))
        mat->bsp_radiance = (value[0] != '0');
    else if (!q_strcasecmp(key, "default_radiance"))
        mat->default_radiance = (float)atof(value);
    else if (!q_strcasecmp(key, "synth_emissive"))
        mat->synth_emissive = (value[0] != '0');
    else if (!q_strcasecmp(key, "emissive_threshold"))
        mat->emissive_threshold = atoi(value);
    else if (!q_strcasecmp(key, "texture_base"))
        q_strlcpy(mat->filename_base, value, sizeof(mat->filename_base));
    else if (!q_strcasecmp(key, "texture_normals"))
        q_strlcpy(mat->filename_normals, value, sizeof(mat->filename_normals));
    else if (!q_strcasecmp(key, "texture_emissive"))
        q_strlcpy(mat->filename_emissive, value, sizeof(mat->filename_emissive));
    else if (!q_strcasecmp(key, "texture_mask"))
        q_strlcpy(mat->filename_mask, value, sizeof(mat->filename_mask));
    else if (!q_strcasecmp(key, "texture_gloss"))
        q_strlcpy(mat->filename_gloss, value, sizeof(mat->filename_gloss));
    else
        Con_DWarning("RT mat: unknown attribute '%s' in material '%s'\n", key, mat->name);
}

// returns number of materials parsed into dest
static int rt_mat_load_file(const char *file_name, rt_material_t *dest, int max_items)
{
    int len = 0;
    char *filebuf = (char *)rt_load_file(file_name, &len);
    if (!filebuf || len == 0)
    {
        if (filebuf)
        {
            rt_load_file_free((byte *)filebuf);
        }
        return 0;
    }

    char linebuf[1024];
    int count = 0;
    int num_materials_in_group = 0;
    int state = 0; // 0=initial, 1=accumulating names, 2=reading params
    const char *ptr = filebuf;
    const char *end = filebuf + len;

    while (ptr < end)
    {
        // read a line
        size_t pos = 0;
        while (ptr < end && *ptr != '\n' && pos + 1 < sizeof(linebuf))
        {
            linebuf[pos++] = *ptr++;
        }
        if (ptr < end && *ptr == '\n')
        {
            ptr++;
        }
        linebuf[pos] = 0;

        // strip comments. '#' starts a comment only at line start or when
        // preceded by whitespace -- Quake 1 warp texture names legitimately
        // contain '#' (e.g. "textures/#lava1")
        char *t = linebuf;
        while (*t == ' ' || *t == '\t')
        {
            t++;
        }
        if (*t == '#')
        {
            linebuf[0] = 0; // full-line comment
        }
        else
        {
            char *hash = strchr(linebuf, '#');
            while (hash)
            {
                if (hash > linebuf && (hash[-1] == ' ' || hash[-1] == '\t'))
                {
                    *hash = 0;
                    break;
                }
                hash = strchr(hash + 1, '#');
            }
        }

        // trim trailing whitespace
        size_t l = strlen(linebuf);
        while (l > 0 && (linebuf[l - 1] == ' ' || linebuf[l - 1] == '\t' || linebuf[l - 1] == '\r'))
        {
            linebuf[--l] = 0;
        }
        if (l == 0)
        {
            continue;
        }

        char last = linebuf[l - 1];
        if (last == ':' || last == ',')
        {
            if (state == 1 || state == 2)
            {
                dest++;
            }
            if (count >= max_items)
            {
                break;
            }
            count++;

            rt_mat_reset(dest);
            linebuf[l - 1] = 0;
            q_strlcpy(dest->name, linebuf, sizeof(dest->name));
            q_strlwr(dest->name);
            dest->valid = true;

            if (state == 1)
            {
                num_materials_in_group++;
            }
            else
            {
                num_materials_in_group = 1;
            }

            state = (last == ',') ? 1 : 2;
            continue;
        }

        // key value pair
        char *key = linebuf;
        char *value = strchr(linebuf, ' ');
        if (!value)
        {
            value = strchr(linebuf, '\t');
        }
        if (!value)
        {
            continue;
        }
        *value++ = 0;
        // trim value
        while (*value == ' ' || *value == '\t')
        {
            value++;
        }

        if (state == 0)
        {
            continue;
        }
        if (state == 1)
        {
            state = 0;
            continue;
        }

        for (int i = 0; i < num_materials_in_group; i++)
        {
            rt_mat_set_attribute(dest - i, key, value);
        }
    }

    rt_load_file_free((byte *)filebuf);
    return count;
}

// ---------------------------------------------------------------------------
// table management
// ---------------------------------------------------------------------------

static void rt_mat_find_dir_mats(int (*cb)(const char *name, void *ctx), void *ctx)
{
    // on-disk materials/*.mat
    char pattern[MAX_OSPATH];
    q_snprintf(pattern, sizeof(pattern), "%s/materials/*.mat", com_gamedir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                continue;
            }
            char name[MAX_QPATH];
            q_snprintf(name, sizeof(name), "materials/%s", fd.cFileName);
            if (cb(name, ctx))
            {
                break;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
}

typedef struct {
    rt_material_t *dest;
    int *count;
    int max;
} rt_mat_load_ctx_t;

static int rt_mat_load_cb(const char *name, void *vctx)
{
    rt_mat_load_ctx_t *ctx = (rt_mat_load_ctx_t *)vctx;
    if (*ctx->count >= ctx->max)
    {
        return 1;
    }
    int loaded = rt_mat_load_file(name, ctx->dest + *ctx->count, ctx->max - *ctx->count);
    *ctx->count += loaded;
    if (loaded > 0)
    {
        Con_Printf("RT: loaded %d materials from %s\n", loaded, name);
    }
    return 0;
}

void RT_MAT_Init(void)
{
    if (rt_initialized)
    {
        return;
    }
    rt_initialized = true;

    Cvar_RegisterVariable(&rt_materials);
    Cvar_RegisterVariable(&rt_mat_debug);

    rt_global_count = 0;
    rt_map_count = 0;

    RT_PKZ_Init();

    rt_mat_load_ctx_t ctx = { rt_global_materials, &rt_global_count, RT_MAT_MAX_GLOBAL };
    // .pkz entries
    RT_PKZ_ListFiles("materials/", ".mat", rt_mat_load_cb, &ctx);
    // on-disk files
    rt_mat_find_dir_mats(rt_mat_load_cb, &ctx);

    rt_mat_cmd = Cmd_AddCommand2("rt_mat", RT_MAT_Cmd, src_command);
}

void RT_MAT_Shutdown(void)
{
    if (!rt_initialized)
    {
        return;
    }
    rt_initialized = false;

    if (rt_mat_cmd)
    {
        Cmd_RemoveCommand(rt_mat_cmd);
        rt_mat_cmd = NULL;
    }
    rt_global_count = 0;
    rt_map_count = 0;
    RT_PKZ_Shutdown();
}

void RT_MAT_ChangeMap(const char *mapname)
{
    if (!rt_initialized)
    {
        return;
    }

    rt_map_count = 0;

    // map-specific material file: <mapname>.mat in materials/
    char name[MAX_QPATH];
    q_snprintf(name, sizeof(name), "materials/%s.mat", mapname);

    rt_mat_load_ctx_t ctx = { rt_map_materials, &rt_map_count, RT_MAT_MAX_MAP };
    rt_mat_load_cb(name, &ctx);
}

// World textures loaded from the BSP (no external HD file) get a name like
// "maps/e1m1.bsp:*lava1" or "maps/e1m1.bsp:e1u1/foo". Convert them to the
// "textures/..." convention used by .mat files (warp '*' becomes '#').
static void rt_mat_normalize_name(const char *name, char *out, size_t outsize)
{
    q_strlcpy(out, name, outsize);

    // find ".bsp" anywhere in the path; everything after the first ':' that
    // follows it is the BSP texture name (may contain sub-dirs, e.g.
    // "maps/e1m1.bsp:e1u1/foo")
    char *bsp = strstr(out, ".bsp");
    if (bsp)
    {
        char *colon = strchr(bsp, ':');
        if (colon)
        {
            char texname[MAX_QPATH];
            q_strlcpy(texname, colon + 1, sizeof(texname));
            if (texname[0] == '*')
            {
                texname[0] = '#'; // warp textures use '#'
            }
            q_snprintf(out, outsize, "textures/%s", texname);
        }
    }

    q_strlwr(out);
}

static rt_material_t *rt_mat_find_in(const char *name, rt_material_t *first, int count)
{
    char n[MAX_QPATH];
    rt_mat_normalize_name(name, n, sizeof(n));
    // strip a trailing file extension, but only if it is really at the end:
    // model skin names like "progs/armor.mdl:frame0" must stay intact
    char *dot = strrchr(n, '.');
    if (dot && !strchr(dot, ':'))
    {
        *dot = 0;
    }

    for (int i = 0; i < count; i++)
    {
        if (first[i].valid)
        {
            if (!strcmp(first[i].name, n))
            {
                if (CVAR_TO_BOOL (rt_mat_debug))
                {
                    Con_Printf("RT: material lookup '%s' -> '%s' FOUND (emissive=%s gloss=%s)\n",
                               name, n,
                               first[i].filename_emissive[0] ? first[i].filename_emissive : "-",
                               first[i].filename_gloss[0] ? first[i].filename_gloss : "-");
                }
                return &first[i];
            }
        }
    }
    if (CVAR_TO_BOOL (rt_mat_debug))
    {
        Con_Printf("RT: material lookup '%s' -> '%s' NOT FOUND\n", name, n);
    }
    return NULL;
}

rt_material_t *RT_MAT_Find(const char *name)
{
    if (!rt_initialized || !RT_MAT_Enabled())
    {
        return NULL;
    }
    if (!name || !name[0])
    {
        return NULL;
    }

    // map materials override global materials (Q2RTX MAT_Find)
    rt_material_t *m = rt_mat_find_in(name, rt_map_materials, rt_map_count);
    if (m)
    {
        return m;
    }
    return rt_mat_find_in(name, rt_global_materials, rt_global_count);
}

// ---------------------------------------------------------------------------
// HD texture pack convention: no .mat files, but every texture set has
// <name>_norm / <name>_gloss / <name>_luma / <name>_glow / <name>_bump.
// ---------------------------------------------------------------------------

static qboolean rt_tex_exists(const char *base)
{
    static const char *exts[] = { "tga", "jpg", "png" };
    for (int e = 0; e < 3; e++)
    {
        char name[MAX_QPATH];
        q_snprintf(name, sizeof(name), "%s.%s", base, exts[e]);

        // .pkz archives: cheap central-directory lookup
        if (RT_PKZ_Exists(name))
        {
            return true;
        }

        // filesystem + .pak
        unsigned int path_id;
        if (COM_FileExists(name, &path_id))
        {
            return true;
        }
    }
    return false;
}

qboolean RT_MAT_AutoDetect(const char *name, rt_material_t *out)
{
    if (!RT_MAT_Enabled())
    {
        return false;
    }
    if (!name || !name[0])
    {
        return false;
    }

    rt_mat_reset(out);
    rt_mat_normalize_name(name, out->name, sizeof(out->name));
    char *dot = strrchr(out->name, '.');
    if (dot && !strchr(dot, ':'))
    {
        *dot = 0;
    }

    char base[MAX_QPATH];
    q_strlcpy(base, out->name, sizeof(base));

    qboolean found = false;

    // normal map: _norm preferred, _bump as fallback
    char suf[MAX_QPATH];
    q_snprintf(suf, sizeof(suf), "%s_norm", base);
    if (rt_tex_exists(suf))
    {
        q_strlcpy(out->filename_normals, suf, sizeof(out->filename_normals));
        found = true;
    }
    else
    {
        q_snprintf(suf, sizeof(suf), "%s_bump", base);
        if (rt_tex_exists(suf))
        {
            q_strlcpy(out->filename_normals, suf, sizeof(out->filename_normals));
            found = true;
        }
    }

    // gloss map -> roughness = 1 - gloss
    q_snprintf(suf, sizeof(suf), "%s_gloss", base);
    if (rt_tex_exists(suf))
    {
        q_strlcpy(out->filename_gloss, suf, sizeof(out->filename_gloss));
        found = true;
    }

    // emissive: _luma preferred, _glow as fallback
    q_snprintf(suf, sizeof(suf), "%s_luma", base);
    if (rt_tex_exists(suf))
    {
        q_strlcpy(out->filename_emissive, suf, sizeof(out->filename_emissive));
        found = true;
    }
    else
    {
        q_snprintf(suf, sizeof(suf), "%s_glow", base);
        if (rt_tex_exists(suf))
        {
            q_strlcpy(out->filename_emissive, suf, sizeof(out->filename_emissive));
            found = true;
        }
    }

    // HD packs carry no alpha/metallic data: keep surfaces non-metallic
    out->metalness_factor = 0.0f;
    out->bump_scale = 1.0f;
    out->emissive_factor = 1.0f;
    out->specular_factor = 1.0f;
    out->base_factor = 1.0f;
    out->kind = RT_MAT_KIND_REGULAR;

    return found;
}

qboolean RT_MAT_Enabled(void)
{
    return rt_materials.value != 0;
}

void RT_MAT_Cmd(void)
{
    if (Cmd_Argc() == 1)
    {
        Con_Printf("rt_materials is %s; %d global, %d map materials\n",
                   RT_MAT_Enabled() ? "ON" : "OFF", rt_global_count, rt_map_count);
        return;
    }

    rt_material_t *m = RT_MAT_Find(Cmd_Argv(1));
    rt_material_t autoMat;
    if (!m && RT_MAT_AutoDetect(Cmd_Argv(1), &autoMat))
    {
        m = &autoMat;
        Con_Printf("(auto-detected from texture suffixes)\n");
    }
    if (!m)
    {
        Con_Printf("no material for '%s'\n", Cmd_Argv(1));
        return;
    }

    Con_Printf("material '%s': base=%s normals=%s emissive=%s gloss=%s mask=%s kind=%d "
               "bump=%.2f rough=%.2f metal=%.2f emiss=%.2f spec=%.2f base=%.2f\n",
               m->name,
               m->filename_base[0] ? m->filename_base : "-",
               m->filename_normals[0] ? m->filename_normals : "-",
               m->filename_emissive[0] ? m->filename_emissive : "-",
               m->filename_gloss[0] ? m->filename_gloss : "-",
               m->filename_mask[0] ? m->filename_mask : "-",
               m->kind, m->bump_scale, m->roughness_override,
               m->metalness_factor, m->emissive_factor, m->specular_factor, m->base_factor);
}
