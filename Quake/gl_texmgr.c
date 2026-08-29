/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers
Copyright (C) 2016 Axel Gneiting

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

// gl_texmgr.c -- fitzquake's texture manager. manages texture images

#include "quakedef.h"
#include "gl_heap.h"
#include "rt_material.h"

#if defined(SDL_FRAMEWORK) || defined(NO_SDL_CONFIG)
#include <SDL2/SDL.h>
#else
#include "SDL.h"
#endif

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_STATIC
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image_resize.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static cvar_t gl_max_size = {"gl_max_size", "0", CVAR_NONE};
static cvar_t gl_picmip = {"gl_picmip", "0", CVAR_NONE};

extern cvar_t vid_filter;
extern cvar_t vid_anisotropic;

extern cvar_t rt_emis_fullbright_dflt;
extern cvar_t rt_brush_rough;
extern cvar_t rt_brush_metal;
extern cvar_t rt_model_rough;
extern cvar_t rt_model_metal;

#define MAX_MIPS 16
static int          numgltextures;
static gltexture_t *active_gltextures, *free_gltextures;
gltexture_t        *notexture, *nulltexture, *whitetexture, *greytexture;

unsigned int d_8to24table[256];
unsigned int d_8to24table_fbright[256];
unsigned int d_8to24table_fbright_fence[256];
#if !RT_RENDERER
unsigned int d_8to24table_nobright[256];
unsigned int d_8to24table_nobright_fence[256];
#endif
unsigned int d_8to24table_conchars[256];
unsigned int d_8to24table_shirt[256];
unsigned int d_8to24table_pants[256];


SDL_mutex *texmgr_mutex;


static RgMaterialCreateFlags TexMgr_GetRtFlags (gltexture_t *glt)
{
	RgMaterialCreateFlags fs = 0;

	if (glt->flags & TEXPREF_MIPMAP)
	{
		fs |= RG_MATERIAL_CREATE_DONT_GENERATE_MIPMAPS_BIT;
	}

	// if controlled by cvar
	if (!(glt->flags & TEXPREF_NEAREST) && !(glt->flags & TEXPREF_LINEAR))
	{
		fs |= RG_MATERIAL_CREATE_DYNAMIC_SAMPLER_FILTER_BIT;
	}

	if (glt->source_format == SRC_LIGHTMAP)
	{
		fs |= RG_MATERIAL_CREATE_UPDATEABLE_BIT;
	}

	return fs;
}

static RgSamplerFilter TexMgr_GetFilterMode (gltexture_t *glt)
{
	if (glt->flags & TEXPREF_NEAREST)
	{
		return RG_SAMPLER_FILTER_NEAREST;
	}

	if (glt->flags & TEXPREF_LINEAR)
	{
		return RG_SAMPLER_FILTER_LINEAR;
	}

	return CVAR_TO_INT32 (vid_filter) == 1 ? RG_SAMPLER_FILTER_NEAREST : RG_SAMPLER_FILTER_LINEAR;
}

static SDL_mutex *rtspecial_mutex;

static THREAD_LOCAL qboolean     rtspecial_started;
static THREAD_LOCAL qboolean     rtspecial_foundfullbright = false;
static THREAD_LOCAL gltexture_t *rtspecial_target = NULL;
static THREAD_LOCAL byte         rtspecial_default_rough;
static THREAD_LOCAL byte         rtspecial_default_metallic;

static THREAD_LOCAL RgMaterialCreateInfo rtspecial_info = {0};
static THREAD_LOCAL void                *rtspecial_info_albedoAlpha = NULL; // to point to data from rtspecial_info
static THREAD_LOCAL char                 rtspecial_info_pRelativePath[MAX_QPATH];

static qboolean TexMgr_ApplyMaterialFromMat (gltexture_t *glt, unsigned *albedoFallback, byte *fullbrightOverride);


void TexMgr_RT_SpecialStart (float default_rough, float default_metallic)
{

	assert (!rtspecial_started && !rtspecial_foundfullbright && rtspecial_target == NULL);
	assert (rtspecial_info_albedoAlpha == NULL);

	rtspecial_started = true;
	rtspecial_default_rough = CLAMP( 0, (int)(default_rough * 255), 255);
	rtspecial_default_metallic = CLAMP (0, (int)(default_metallic * 255), 255);
}

static void TexMgr_RT_SpecialSave (gltexture_t *glt, const RgMaterialCreateInfo *info)
{
	assert (rtspecial_info_albedoAlpha == NULL);

	rtspecial_target = glt;
	rtspecial_info = *info;

	{
		size_t sz = sizeof (uint32_t) * glt->width * glt->height;

		rtspecial_info_albedoAlpha = Mem_Alloc (sz);
		memcpy (rtspecial_info_albedoAlpha, info->textures.pDataAlbedoAlpha, sz);
	}

	if (info->pRelativePath)
	{
		q_strlcpy (rtspecial_info_pRelativePath, info->pRelativePath, sizeof (rtspecial_info_pRelativePath));
	}
	else
	{
		rtspecial_info_pRelativePath[0] = '\0';
	}
}

static byte Luminance (byte r, byte g, byte b)
{
	float l = 0.2126f * (float)r / 255.0f + 0.7152f * (float)g / 255.0f + 0.0722f * (float)b / 255.0f;
	int   i = (int)(l * 255);
	    
	return q_min (i, 255);
}

// https://gist.github.com/marukrap/7c361f2c367eaf40537a8715e3fd952a
void RGBtoHSV (const vec3_t rgb, vec3_t out_hsv)
{
	float R = CLAMP (0.0f, rgb[0], 1.0f);
	float G = CLAMP (0.0f, rgb[1], 1.0f);
	float B = CLAMP (0.0f, rgb[2], 1.0f);

	float M = max (R, max (G, B));
	float m = min (R, min (G, B));
	float C = M - m; // Chroma

	float H = 0.f; // Hue
	float S = 0.f; // Saturation
	float V = 0.f; // Value

	if (C != 0.f)
	{
		if (M == R)
			H = fmodf (((G - B) / C), 6.f);
		else if (M == G)
			H = ((B - R) / C) + 2;
		else if (M == B)
			H = ((R - G) / C) + 4;

		H *= 60;
	}

	if (H < 0.f)
		H += 360;

	V = M;

	if (V != 0.f)
		S = C / V;

	out_hsv[0] = CLAMP (0.0f, H, 360.0f);
	out_hsv[1] = CLAMP (0.0f, S, 1.0f);
	out_hsv[2] = CLAMP (0.0f, V, 1.0f);
}

void HSVtoRGB (const vec3_t hsv, vec3_t out_rgb)
{
	float H = CLAMP (0.0f, hsv[0], 360.0f);
	float S = CLAMP (0.0f, hsv[1], 1.0f);
	// Note: don't clamp, as we modify it
	float V = max (0.0f, hsv[2]);

	float C = S * V;                        // Chroma
	float HPrime = fmodf (H / 60, 6.f); // H'
	float X = C * (1 - fabsf (fmodf (HPrime, 2.f) - 1));
	float M = V - C;

	float R = 0.f;
	float G = 0.f;
	float B = 0.f;

	switch ((int)HPrime)
	{
	case 0:
		R = C;
		G = X;
		break; // [0, 1)
	case 1:
		R = X;
		G = C;
		break; // [1, 2)
	case 2:
		G = C;
		B = X;
		break; // [2, 3)
	case 3:
		G = X;
		B = C;
		break; // [3, 4)
	case 4:
		R = X;
		B = C;
		break; // [4, 5)
	case 5:
		R = C;
		B = X;
		break; // [5, 6)
	default:
		break;
	}

	R += M;
	G += M;
	B += M;

	// Note: don't clamp, as we modify luminance
	out_rgb[0] = max (0.0f, R);
	out_rgb[1] = max (0.0f, G);
	out_rgb[2] = max (0.0f, B);
}
static void ModifyColorValue (vec3_t inout_color, float target_value)
{
	vec3_t hsv;
	RGBtoHSV (inout_color, hsv);

	hsv[2] *= target_value;

	HSVtoRGB (hsv, inout_color);
}

static void FullbrightToRME (unsigned width, unsigned height, byte *fullbright)
{
	size_t pixels = (size_t)width * (size_t)height;

	while (pixels-- > 0)
	{
		byte lum = Luminance (fullbright[0], fullbright[1], fullbright[2]);

		if (lum > 0)
		{
			lum = CLAMP (0, CVAR_TO_UINT32 (rt_emis_fullbright_dflt), 255);
		}
		else
		{
			lum = 0;
		}

		// rough
		fullbright[0] = rtspecial_default_rough;
		// metallic
		fullbright[1] = rtspecial_default_metallic;
		// emissive
		fullbright[2] = lum;

		fullbright += 4;
	}
}

static void TexMgr_RT_SpecialFullbright (unsigned width, unsigned height, uint32_t *fullbright)
{
	assert (rtspecial_target != NULL && rtspecial_info_albedoAlpha != NULL);
	assert (rtspecial_info.size.width > 0 && rtspecial_info.size.height > 0);

	// strange vkpt limitation
	if (rtspecial_info.size.width != width || rtspecial_info.size.height != height)
	{
		Con_DWarning ("Ignoring fullbright of \"%s\", as it has different size with albedo", rtspecial_info_pRelativePath);
		assert (0);
		return;
	}

	rtspecial_foundfullbright = true;

	FullbrightToRME (width, height, (byte *)fullbright);

	// If the base already has a Q2RTX-style .mat material (phase 4.5, applied
	// when the base texture was loaded), rebuild it with the classic fullbright
	// mask merged into the emissive channel. This keeps the .mat normals/
	// gloss while letting fullbright pixels (buttons, light panels, runes)
	// emit light -- without this, the .mat synthesis would overwrite the
	// emissive material with emiss = 0 for these textures.
	if (rtspecial_target->rtmaterial != RG_NULL_HANDLE)
	{
		if (TexMgr_ApplyMaterialFromMat (rtspecial_target, (unsigned *)rtspecial_info_albedoAlpha, (byte *)fullbright))
			return;
	}

	// average emitted color (albedo * fullbright emission) for emissive area
	// lights (buttons, light panels, runes that use the classic fullbright mask
	// and have no Q2RTX .mat definition)
	{
		const byte *alb = (const byte *)rtspecial_info_albedoAlpha;
		const byte *fb  = (const byte *)fullbright;
		const size_t npix = (size_t)width * height;
		double emR = 0.0, emG = 0.0, emB = 0.0;
		for (size_t i = 0; i < npix; i++)
		{
			const float e = fb[i * 4 + 2] / 255.0f;
			if (e > 0.0f)
			{
				emR += alb[i * 4 + 0] * e;
				emG += alb[i * 4 + 1] * e;
				emB += alb[i * 4 + 2] * e;
			}
		}
		if (emR > 0.0 || emG > 0.0 || emB > 0.0)
		{
			rtspecial_target->rtemissive = true;
			rtspecial_target->rtemissivecolor[0] = emR / (npix * 255.0);
			rtspecial_target->rtemissivecolor[1] = emG / (npix * 255.0);
			rtspecial_target->rtemissivecolor[2] = emB / (npix * 255.0);
		}
	}

	rtspecial_info.textures.pDataAlbedoAlpha = rtspecial_info_albedoAlpha;
	rtspecial_info.pRelativePath = rtspecial_info_pRelativePath;

	rtspecial_info.textures.pDataRoughnessMetallicEmission = fullbright;

    SDL_LockMutex (rtspecial_mutex);
	RgResult r = rgCreateMaterial (vulkan_globals.instance, &rtspecial_info, &rtspecial_target->rtmaterial);
	RG_CHECK (r);
	SDL_UnlockMutex (rtspecial_mutex);
}

void TexMgr_RT_SpecialEnd ()
{
	assert (rtspecial_started);
	assert (rtspecial_target != NULL && rtspecial_info_albedoAlpha != NULL);

	if (!rtspecial_foundfullbright && rtspecial_target->rtmaterial == RG_NULL_HANDLE)
	{
		rtspecial_info.textures.pDataAlbedoAlpha = rtspecial_info_albedoAlpha;
		rtspecial_info.pRelativePath = rtspecial_info_pRelativePath;

		SDL_LockMutex (rtspecial_mutex);
		RgResult r = rgCreateMaterial (vulkan_globals.instance, &rtspecial_info, &rtspecial_target->rtmaterial);
		RG_CHECK (r);
		SDL_UnlockMutex (rtspecial_mutex);

	}

	Mem_Free (rtspecial_info_albedoAlpha);
	
	rtspecial_started=false;
	rtspecial_target = NULL;
	rtspecial_foundfullbright = false;
	memset (&rtspecial_info, 0, sizeof (rtspecial_info));
	rtspecial_info_albedoAlpha = NULL;
	rtspecial_info_pRelativePath[0] = '\0';

}

/*
================================================================================

    COMMANDS

================================================================================
*/

/*
===============
TexMgr_Imagelist_f -- report loaded textures
===============
*/
static void TexMgr_Imagelist_f (void)
{
	float        mb;
	float        texels = 0;
	gltexture_t *glt;

	for (glt = active_gltextures; glt; glt = glt->next)
	{
		Con_SafePrintf ("   %4i x%4i %s\n", glt->width, glt->height, glt->name);
		if (glt->flags & TEXPREF_MIPMAP)
			texels += glt->width * glt->height * 4.0f / 3.0f;
		else
			texels += (glt->width * glt->height);
	}

	mb = (texels * 4) / 0x100000;
	Con_Printf ("%i textures %i pixels %1.1f megabytes\n", numgltextures, (int)texels, mb);
}

/*
================================================================================

    TEXTURE MANAGER

================================================================================
*/

/*
================
TexMgr_FindTexture
================
*/
gltexture_t *TexMgr_FindTexture (qmodel_t *owner, const char *name)
{
	SDL_LockMutex (texmgr_mutex);
	gltexture_t *glt = NULL;

	if (name)
	{
		for (glt = active_gltextures; glt; glt = glt->next)
		{
			if (glt->owner == owner && !strcmp (glt->name, name))
				goto unlock_mutex;
		}
	}

unlock_mutex:
	SDL_UnlockMutex (texmgr_mutex);
	return glt;
}

/*
================
TexMgr_NewTexture
================
*/
gltexture_t *TexMgr_NewTexture (void)
{
	SDL_LockMutex (texmgr_mutex);
	gltexture_t *glt;

	glt = free_gltextures;
	free_gltextures = glt->next;
	glt->next = active_gltextures;
	active_gltextures = glt;

	numgltextures++;
	SDL_UnlockMutex (texmgr_mutex);
	return glt;
}

static void GL_DeleteTexture (gltexture_t *texture);

/*
================
TexMgr_FreeTexture
================
*/
void TexMgr_FreeTexture (gltexture_t *kill)
{
	SDL_LockMutex (texmgr_mutex);
	gltexture_t *glt;

	if (kill == NULL)
	{
		Con_Printf ("TexMgr_FreeTexture: NULL texture\n");
		goto unlock_mutex;
	}

	if (active_gltextures == kill)
	{
		active_gltextures = kill->next;
		kill->next = free_gltextures;
		free_gltextures = kill;

		GL_DeleteTexture (kill);
		numgltextures--;
		goto unlock_mutex;
	}

	for (glt = active_gltextures; glt; glt = glt->next)
	{
		if (glt->next == kill)
		{
			glt->next = kill->next;
			kill->next = free_gltextures;
			free_gltextures = kill;

			GL_DeleteTexture (kill);
			numgltextures--;
			goto unlock_mutex;
		}
	}

	Con_Printf ("TexMgr_FreeTexture: not found\n");
unlock_mutex:
	SDL_UnlockMutex (texmgr_mutex);
}

/*
================
TexMgr_FreeTextures

compares each bit in "flags" to the one in glt->flags only if that bit is active in "mask"
================
*/
void TexMgr_FreeTextures (unsigned int flags, unsigned int mask)
{
	SDL_LockMutex (texmgr_mutex);
	gltexture_t *glt, *next;

	for (glt = active_gltextures; glt; glt = next)
	{
		next = glt->next;
		if ((glt->flags & mask) == (flags & mask))
			TexMgr_FreeTexture (glt);
	}
	SDL_UnlockMutex (texmgr_mutex);
}

/*
================
TexMgr_FreeTexturesForOwner
================
*/
void TexMgr_FreeTexturesForOwner (qmodel_t *owner)
{
	SDL_LockMutex (texmgr_mutex);
	gltexture_t *glt, *next;

	for (glt = active_gltextures; glt; glt = next)
	{
		next = glt->next;
		if (glt && glt->owner == owner)
			TexMgr_FreeTexture (glt);
	}
	SDL_UnlockMutex (texmgr_mutex);
}

/*
================
TexMgr_DeleteTextureObjects
================
*/
void TexMgr_DeleteTextureObjects (void)
{
	SDL_LockMutex (texmgr_mutex);
	gltexture_t *glt;

	for (glt = active_gltextures; glt; glt = glt->next)
		GL_DeleteTexture (glt);
	SDL_UnlockMutex (texmgr_mutex);
}

/*
================================================================================

    INIT

================================================================================
*/

/*
=================
TexMgr_LoadPalette -- johnfitz -- was VID_SetPalette, moved here, renamed, rewritten
=================
*/
void TexMgr_LoadPalette (void)
{
	byte *src, *dst;
	int   i;
	FILE *f;

	COM_FOpenFile ("gfx/palette.lmp", &f, NULL);
	if (!f)
		Sys_Error ("Couldn't load gfx/palette.lmp");

	byte pal[768];
	if (fread (pal, 1, 768, f) != 768)
		Sys_Error ("Couldn't load gfx/palette.lmp");
	fclose (f);

	// standard palette, 255 is transparent
	dst = (byte *)d_8to24table;
	src = pal;
	for (i = 0; i < 256; i++)
	{
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = 255;
	}
	((byte *)&d_8to24table[255])[3] = 0;

	// fullbright palette, 0-223 are black (for additive blending)
	src = pal + 224 * 3;
	dst = (byte *)&d_8to24table_fbright[224];
	for (i = 224; i < 256; i++)
	{
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = 255;
	}
	for (i = 0; i < 224; i++)
	{
		dst = (byte *)&d_8to24table_fbright[i];
		dst[3] = 255;
		dst[2] = dst[1] = dst[0] = 0;
	}

#if !RT_RENDERER
	// nobright palette, 224-255 are black (for additive blending)
	dst = (byte *)d_8to24table_nobright;
	src = pal;
	for (i = 0; i < 256; i++)
	{
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = 255;
	}
	for (i = 224; i < 256; i++)
	{
		dst = (byte *)&d_8to24table_nobright[i];
		dst[3] = 255;
		dst[2] = dst[1] = dst[0] = 0;
	}
#endif

	// fullbright palette, for fence textures
	memcpy (d_8to24table_fbright_fence, d_8to24table_fbright, 256 * 4);
	d_8to24table_fbright_fence[255] = 0; // Alpha of zero.

#if !RT_RENDERER
	// nobright palette, for fence textures
	memcpy (d_8to24table_nobright_fence, d_8to24table_nobright, 256 * 4);
	d_8to24table_nobright_fence[255] = 0; // Alpha of zero.
#endif

	// conchars palette, 0 and 255 are transparent
	memcpy (d_8to24table_conchars, d_8to24table, 256 * 4);
	((byte *)&d_8to24table_conchars[0])[3] = 0;
}

/*
================
TexMgr_NewGame
================
*/
void TexMgr_NewGame (void)
{
	TexMgr_FreeTextures (0, TEXPREF_PERSIST); // deletes all textures where TEXPREF_PERSIST is unset
	TexMgr_LoadPalette ();
}

/*
================
TexMgr_Init

must be called before any texture loading
================
*/
void TexMgr_Init (void)
{
	int               i;
	static byte       notexture_data[16] = {159, 91, 83, 255, 0, 0, 0, 255, 0, 0, 0, 255, 159, 91, 83, 255};                    // black and pink checker
	static byte       nulltexture_data[16] = {127, 191, 255, 255, 0, 0, 0, 255, 0, 0, 0, 255, 127, 191, 255, 255};              // black and blue checker
	static byte       whitetexture_data[16] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}; // white
	static byte       greytexture_data[16] = {127, 127, 127, 255, 127, 127, 127, 255, 127, 127, 127, 255, 127, 127, 127, 255};  // 50% grey
	extern texture_t *r_notexture_mip, *r_notexture_mip2;

	texmgr_mutex = SDL_CreateMutex ();
	rtspecial_mutex = SDL_CreateMutex ();

	// init texture list
	free_gltextures = (gltexture_t *)Mem_Alloc (MAX_GLTEXTURES * sizeof (gltexture_t));
	active_gltextures = NULL;
	for (i = 0; i < MAX_GLTEXTURES - 1; i++)
		free_gltextures[i].next = &free_gltextures[i + 1];
	free_gltextures[i].next = NULL;
	numgltextures = 0;

	// palette
	TexMgr_LoadPalette ();

	Cvar_RegisterVariable (&gl_max_size);
	Cvar_RegisterVariable (&gl_picmip);
	Cmd_AddCommand ("imagelist", &TexMgr_Imagelist_f);

	// load notexture images
	notexture = TexMgr_LoadImage (
		NULL, NULL, "notexture", 2, 2, SRC_RGBA, notexture_data, "", (src_offset_t)notexture_data, TEXPREF_NEAREST | TEXPREF_PERSIST | TEXPREF_NOPICMIP);
	nulltexture = TexMgr_LoadImage (
		NULL, NULL, "nulltexture", 2, 2, SRC_RGBA, nulltexture_data, "", (src_offset_t)nulltexture_data, TEXPREF_NEAREST | TEXPREF_PERSIST | TEXPREF_NOPICMIP);
	whitetexture = TexMgr_LoadImage (
		NULL, NULL, "whitetexture", 2, 2, SRC_RGBA, whitetexture_data, "", (src_offset_t)whitetexture_data, TEXPREF_NEAREST | TEXPREF_PERSIST | TEXPREF_NOPICMIP);
	greytexture = TexMgr_LoadImage (
		NULL, NULL, "greytexture", 2, 2, SRC_RGBA, greytexture_data, "", (src_offset_t)greytexture_data, TEXPREF_NEAREST | TEXPREF_PERSIST | TEXPREF_NOPICMIP);

	// have to assign these here becuase Mod_Init is called before TexMgr_Init
	r_notexture_mip->gltexture = r_notexture_mip2->gltexture = notexture;
}

/*
================================================================================

    IMAGE LOADING

================================================================================
*/

/*
================
TexMgr_Downsample
================
*/
static unsigned *TexMgr_Downsample (unsigned *data, int in_width, int in_height, int out_width, int out_height)
{
	const int out_size_bytes = out_width * out_height * 4;

	assert ((out_width >= 1) && (out_width < in_width));
	assert ((out_height >= 1) && (out_height < in_height));

	byte *image_resize_buffer;
	TEMP_ALLOC (byte, image_resize_buffer, out_size_bytes);
	stbir_resize_uint8 ((byte *)data, in_width, in_height, 0, image_resize_buffer, out_width, out_height, 0, 4);
	memcpy (data, image_resize_buffer, out_size_bytes);
	TEMP_FREE (image_resize_buffer);

	return data;
}

/*
===============
TexMgr_AlphaEdgeFix

eliminate pink edges on sprites, etc.
operates in place on 32bit data
===============
*/
static void TexMgr_AlphaEdgeFix (byte *data, int width, int height)
{
	int   i, j, n = 0, b, c[3] = {0, 0, 0}, lastrow, thisrow, nextrow, lastpix, thispix, nextpix;
	byte *dest = data;

	for (i = 0; i < height; i++)
	{
		lastrow = width * 4 * ((i == 0) ? height - 1 : i - 1);
		thisrow = width * 4 * i;
		nextrow = width * 4 * ((i == height - 1) ? 0 : i + 1);

		for (j = 0; j < width; j++, dest += 4)
		{
			if (dest[3]) // not transparent
				continue;

			lastpix = 4 * ((j == 0) ? width - 1 : j - 1);
			thispix = 4 * j;
			nextpix = 4 * ((j == width - 1) ? 0 : j + 1);

			b = lastrow + lastpix;
			if (data[b + 3])
			{
				c[0] += data[b];
				c[1] += data[b + 1];
				c[2] += data[b + 2];
				n++;
			}
			b = thisrow + lastpix;
			if (data[b + 3])
			{
				c[0] += data[b];
				c[1] += data[b + 1];
				c[2] += data[b + 2];
				n++;
			}
			b = nextrow + lastpix;
			if (data[b + 3])
			{
				c[0] += data[b];
				c[1] += data[b + 1];
				c[2] += data[b + 2];
				n++;
			}
			b = lastrow + thispix;
			if (data[b + 3])
			{
				c[0] += data[b];
				c[1] += data[b + 1];
				c[2] += data[b + 2];
				n++;
			}
			b = nextrow + thispix;
			if (data[b + 3])
			{
				c[0] += data[b];
				c[1] += data[b + 1];
				c[2] += data[b + 2];
				n++;
			}
			b = lastrow + nextpix;
			if (data[b + 3])
			{
				c[0] += data[b];
				c[1] += data[b + 1];
				c[2] += data[b + 2];
				n++;
			}
			b = thisrow + nextpix;
			if (data[b + 3])
			{
				c[0] += data[b];
				c[1] += data[b + 1];
				c[2] += data[b + 2];
				n++;
			}
			b = nextrow + nextpix;
			if (data[b + 3])
			{
				c[0] += data[b];
				c[1] += data[b + 1];
				c[2] += data[b + 2];
				n++;
			}

			// average all non-transparent neighbors
			if (n)
			{
				dest[0] = (byte)(c[0] / n);
				dest[1] = (byte)(c[1] / n);
				dest[2] = (byte)(c[2] / n);

				n = c[0] = c[1] = c[2] = 0;
			}
		}
	}
}

/*
================
TexMgr_8to32
================
*/
static void TexMgr_8to32 (byte *in, unsigned *out, int pixels, unsigned int *usepal)
{
	for (int i = 0; i < pixels; i++)
		*out++ = usepal[*in++];
}

/*
================
TexMgr_DeriveNumMips
================
*/
static int TexMgr_DeriveNumMips (int width, int height)
{
	int num_mips = 0;
	while (width >= 1 && height >= 1)
	{
		width /= 2;
		height /= 2;
		num_mips += 1;
	}
	return num_mips;
}

/*
================
TexMgr_DeriveStagingSize
================
*/
static int TexMgr_DeriveStagingSize (int width, int height)
{
	int size = 0;
	while (width >= 1 && height >= 1)
	{
		size += width * height * 4;
		width /= 2;
		height /= 2;
	}
	return size;
}

/*
================
TexMgr_PreMultiply32
================
*/
static void TexMgr_PreMultiply32 (byte *in, size_t width, size_t height)
{
	size_t pixels = width * height;
	while (pixels-- > 0)
	{
		in[0] = ((int)in[0] * (int)in[3]) >> 8;
		in[1] = ((int)in[1] * (int)in[3]) >> 8;
		in[2] = ((int)in[2] * (int)in[3]) >> 8;
		in += 4;
	}
}

/*
================
TexMgr_LoadImage32 -- handles 32bit source data
================
*/
static void TexMgr_LoadImage32 (gltexture_t *glt, unsigned *data)
{
	GL_DeleteTexture (glt);

	// do this before any rescaling
	if (glt->flags & TEXPREF_PREMULTIPLY)
		TexMgr_PreMultiply32 ((byte *)data, glt->width, glt->height);

	// mipmap down
	int picmip = (glt->flags & TEXPREF_NOPICMIP) ? 0 : q_max ((int)gl_picmip.value, 0);
	int mipwidth = q_max (glt->width >> picmip, 1);
	int mipheight = q_max (glt->height >> picmip, 1);

	int maxsize = 4096;
	if ((mipwidth > maxsize) || (mipheight > maxsize))
	{
		if (mipwidth >= mipheight)
		{
			mipheight = q_max ((mipheight * maxsize) / mipwidth, 1);
			mipwidth = maxsize;
		}
		else
		{
			mipwidth = q_max ((mipwidth * maxsize) / mipheight, 1);
			mipheight = maxsize;
		}
	}

	if ((int)glt->width != mipwidth || (int)glt->height != mipheight)
	{
		TexMgr_Downsample (data, glt->width, glt->height, mipwidth, mipheight);
		glt->width = mipwidth;
		glt->height = mipheight;
		if (glt->flags & TEXPREF_ALPHA)
			TexMgr_AlphaEdgeFix ((byte *)data, glt->width, glt->height);
	}
	int num_mips = (glt->flags & TEXPREF_MIPMAP) ? TexMgr_DeriveNumMips (glt->width, glt->height) : 1;

	SDL_LockMutex (texmgr_mutex);
	const qboolean warp_image = (glt->flags & TEXPREF_WARPIMAGE);
	if (warp_image)
		num_mips = WARPIMAGEMIPS;

	// Check for sanity. This should never be reached.
	if (num_mips > MAX_MIPS)
		Sys_Error ("Texture has over %d mips", MAX_MIPS);

	// const qboolean lightmap = glt->source_format == SRC_LIGHTMAP;
	// const qboolean surface_indices = glt->source_format == SRC_SURF_INDICES;

	// const VkFormat format = !surface_indices ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R32_UINT;


	RgMaterialCreateInfo info = {
		.flags = TexMgr_GetRtFlags (glt),
		.size = {glt->width, glt->height},
		.textures =
			{
				.pDataAlbedoAlpha = data,
				.pDataRoughnessMetallicEmission = NULL,
				.pDataNormal = NULL,
			},
		.pRelativePath = glt->rtname,
		.filter = TexMgr_GetFilterMode (glt),
		.addressModeU = RG_SAMPLER_ADDRESS_MODE_REPEAT,
		.addressModeV = RG_SAMPLER_ADDRESS_MODE_REPEAT,
	};

	if (!rtspecial_started)
	{
		SDL_LockMutex (rtspecial_mutex);
	    RgResult r = rgCreateMaterial (vulkan_globals.instance, &info, &glt->rtmaterial);
	    RG_CHECK (r);
		SDL_UnlockMutex (rtspecial_mutex);
	}
	else
	{
		if (glt->flags & TEXPREF_RT_IS_EMISSIVE)
		{
			TexMgr_RT_SpecialFullbright (glt->width, glt->height, data);
		}
		else
		{
		    TexMgr_RT_SpecialSave (glt, &info);
		}
	}

	// Q2RTX-style .mat material (phase 4.5): if this texture has a material
	// definition, replace the RT material with the synthesized PBR one
	// (albedo from texture_base, RME from base.alpha/roughness, normal.alpha
	// metallic and emissive, normal from texture_normals).
	TexMgr_ApplyMaterialFromMat (glt, data, NULL);

	SDL_UnlockMutex (texmgr_mutex);
}

/*
================
TexMgr_ApplyMaterialFromMat

Builds the vkpt RGBA8 material textures from a Q2RTX-style .mat definition
(phase 4.5) and replaces glt->rtmaterial with the result. Returns true if a
material was applied.
================
*/
static qboolean TexMgr_ApplyMaterialFromMat (gltexture_t *glt, unsigned *albedoFallback, byte *fullbrightOverride)
{
	// The material key is the texture file path without extension, e.g.
	// "textures/e1u1/foo" -- this matches the .yaml entry names (Q2RTX
	// convention). Note: glt->rtname is the vkpt override path ("maps/...")
	// and must NOT be used here.
	rt_material_t *mat = RT_MAT_Find (glt->name);
	if (!mat)
		return false;

	// Apply the explicit "is_light" flag up front: materials.yaml entries that
	// only carry "is_light: true" (no PBR textures -- e.g. model skins like
	// "progs/flame2.mdl:frame0") hit the early-return below, so the flag must
	// be set before it, otherwise r_alias.c never generates their emissive
	// spherical lights.
	glt->rtislight = mat->is_light;

	// Migrated texture_custom_info.txt flags (authored in materials.yaml).
	// These must also be applied before the early-return below: many of the
	// affected textures (projectiles, laser bolts, window glass, flat-shaded
	// viewmodels) carry no PBR textures and would otherwise miss their flags.
	if (mat->has_light_color)
	{
		VectorCopy (mat->light_color, glt->rtlightcolor);
		glt->rthaslightcolor = true;
		if (mat->light_brightness != 1.0f)
			ModifyColorValue (glt->rtlightcolor, mat->light_brightness);
	}
	glt->rtupoffset = mat->light_upoffset;
	glt->rtmirror = mat->mirror;
	glt->rtexactnormals = mat->exact_normals;
	glt->rtforcerasterize = mat->force_rasterize;

	const int tw = glt->width;
	const int th = glt->height;
	const int npix = tw * th;

	// load and resize the material textures to the final size
	int bw = 0, bh = 0;
	byte *baseTex = RT_MAT_LoadTexture (mat, RT_MAT_TEX_BASE, &bw, &bh);
	int nw = 0, nh = 0;
	byte *normTex = RT_MAT_LoadTexture (mat, RT_MAT_TEX_NORMALS, &nw, &nh);
	int ew = 0, eh = 0;
	byte *emisTex = RT_MAT_LoadTexture (mat, RT_MAT_TEX_EMISSIVE, &ew, &eh);
	glt->rtemissivetex = (emisTex != NULL);
	int gw = 0, gh = 0;
	byte *glossTex = RT_MAT_LoadTexture (mat, RT_MAT_TEX_GLOSS, &gw, &gh);

	byte *baseBuf = NULL, *normBuf = NULL, *emisBuf = NULL, *glossBuf = NULL;
	if (baseTex) { baseBuf = (byte *)Mem_Alloc (npix * 4); stbir_resize_uint8 (baseTex, bw, bh, 0, baseBuf, tw, th, 0, 4); Mem_Free (baseTex); }
	if (normTex) { normBuf = (byte *)Mem_Alloc (npix * 4); stbir_resize_uint8 (normTex, nw, nh, 0, normBuf, tw, th, 0, 4); Mem_Free (normTex); }
	if (emisTex) { emisBuf = (byte *)Mem_Alloc (npix * 4); stbir_resize_uint8 (emisTex, ew, eh, 0, emisBuf, tw, th, 0, 4); Mem_Free (emisTex); }
	if (glossTex) { glossBuf = (byte *)Mem_Alloc (npix * 4); stbir_resize_uint8 (glossTex, gw, gh, 0, glossBuf, tw, th, 0, 4); Mem_Free (glossTex); }

	// if the material specifies no base texture, fall back to the original one
	if (!baseBuf && !albedoFallback)
	{
		if (normBuf) Mem_Free (normBuf);
		if (emisBuf) Mem_Free (emisBuf);
		if (glossBuf) Mem_Free (glossBuf);
		return false;
	}

	// does the base texture carry a real alpha channel (Q2RTX-style roughness
	// packed in alpha)? JPGs have alpha = 255 everywhere, so they don't count.
	qboolean baseHasAlpha = false;
	if (baseBuf)
	{
		for (int i = 0; i < npix; i++)
		{
			if (baseBuf[i * 4 + 3] != 255)
			{
				baseHasAlpha = true;
				break;
			}
		}
	}

	// same for the normal map: metalness is packed in its alpha (Q2RTX
	// convention), but such PNGs have no alpha -> stay non-metallic.
	qboolean normHasAlpha = false;
	if (normBuf)
	{
		for (int i = 0; i < npix; i++)
		{
			if (normBuf[i * 4 + 3] != 255)
			{
				normHasAlpha = true;
				break;
			}
		}
	}

	byte *albedo = (byte *)Mem_Alloc (npix * 4);
	byte *rme    = (byte *)Mem_Alloc (npix * 4);
	byte *normal = (byte *)Mem_Alloc (npix * 4);

	const float baseFactor = (mat->base_factor > 0.0f) ? mat->base_factor : 1.0f;
	const float roughOverride = mat->roughness_override; // 0 = use map-based roughness

	// Fallback roughness for materials with no explicit PBR data (bare
	// materials.yaml entries for model skins, etc.). The brush world's
	// TexMgr_RT_SpecialStart thread-locals are only valid on the thread that
	// happens to run the world texture task, so model textures synthesized
	// elsewhere (e.g. main-thread single-skin loads) would otherwise get a
	// stale rough=0 -- i.e. perfectly mirrored surfaces. Derive the default
	// from the owner model type instead, matching the cvar used when the
	// geometry is uploaded (r_world.c / r_alias.c / r_sprite.c).
	const qboolean isBrush = glt->owner && glt->owner->type == mod_brush;
	const float defaultRough = isBrush ? CVAR_TO_FLOAT (rt_brush_rough) : CVAR_TO_FLOAT (rt_model_rough);

	// average emitted color (albedo * emissive), used to generate emissive
	// area lights (Q2RTX-style triangle lights) in r_world.c
	glt->rtemissive = false;
	glt->rtemissivecolor[0] = 0.0f;
	glt->rtemissivecolor[1] = 0.0f;
	glt->rtemissivecolor[2] = 0.0f;
	glt->rtemissivemean = 0.0f;
	glt->rtislight = false;
	float emissR = 0.0f, emissG = 0.0f, emissB = 0.0f;
	double emissMean = 0.0;

	for (int i = 0; i < npix; i++)
	{
		// albedo (sRGB), alpha = opaque (or mask later)
		const byte *src = baseBuf ? baseBuf + i * 4 : (byte *)albedoFallback + i * 4;
		int r = (int)(src[0] * baseFactor);
		int g = (int)(src[1] * baseFactor);
		int b = (int)(src[2] * baseFactor);
		albedo[i * 4 + 0] = CLAMP (0, r, 255);
		albedo[i * 4 + 1] = CLAMP (0, g, 255);
		albedo[i * 4 + 2] = CLAMP (0, b, 255);
		albedo[i * 4 + 3] = 255;

		// roughness: roughness_override > gloss map (1 - gloss) > base alpha
		// (Q2RTX packing) > default
		float rough;
		if (roughOverride > 0.0f)
			rough = roughOverride;
		else if (glossBuf)
			rough = 1.0f - glossBuf[i * 4] / 255.0f;
		else if (baseHasAlpha)
			rough = baseBuf[i * 4 + 3] / 255.0f;
		else
			rough = defaultRough;

		// metallic: from the normal map alpha (if it has one), or the
		// metalness_factor from the .mat
		float metal = mat->metalness_factor;
		if (normBuf && normHasAlpha)
			metal = (normBuf[i * 4 + 3] / 255.0f) * mat->metalness_factor;

		// emissive: from the emissive texture, synthesized from the base, or
		// merged from the classic fullbright mask (fullbrightOverride)
		float emiss = 0.0f;
		if (emisBuf)
		{
			emiss = (0.2126f * emisBuf[i * 4 + 0] + 0.7152f * emisBuf[i * 4 + 1] + 0.0722f * emisBuf[i * 4 + 2]) / 255.0f;
			emiss *= mat->emissive_factor;
		}
		if (fullbrightOverride)
		{
			// classic fullbright mask in RME layout (after FullbrightToRME):
			// channel 2 carries the emission value
			const float fb = fullbrightOverride[i * 4 + 2] / 255.0f;
			if (fb > emiss)
				emiss = fb;
		}
		else if (!emisBuf && mat->synth_emissive)
		{
			const float lum = (0.2126f * src[0] + 0.7152f * src[1] + 0.0722f * src[2]) / 255.0f;
			if (mat->emissive_threshold <= 0 || lum > mat->emissive_threshold / 255.0f)
				emiss = lum * mat->emissive_factor;
		}

		// accumulate the average emitted color (albedo * emissive) for the
		// emissive area-light generation
		if (emiss > 0.0f)
		{
			emissR += albedo[i * 4 + 0] * emiss;
			emissG += albedo[i * 4 + 1] * emiss;
			emissB += albedo[i * 4 + 2] * emiss;
		}
		emissMean += emiss;

		rme[i * 4 + 0] = CLAMP (0, (int)(rough * 255), 255);
		rme[i * 4 + 1] = CLAMP (0, (int)(metal * 255), 255);
		rme[i * 4 + 2] = CLAMP (0, (int)(emiss * 255), 255);
		rme[i * 4 + 3] = 255;

		// normal map (bump_scale applied around 128)
		if (normBuf)
		{
			float nx = (normBuf[i * 4 + 0] - 128.0f) * mat->bump_scale + 128.0f;
			float ny = (normBuf[i * 4 + 1] - 128.0f) * mat->bump_scale + 128.0f;
			normal[i * 4 + 0] = CLAMP (0, (int)nx, 255);
			normal[i * 4 + 1] = CLAMP (0, (int)ny, 255);
			normal[i * 4 + 2] = normBuf[i * 4 + 2];
		}
		else
		{
			normal[i * 4 + 0] = 128;
			normal[i * 4 + 1] = 128;
			normal[i * 4 + 2] = 255;
		}
		normal[i * 4 + 3] = 255;
	}

	if (baseBuf) Mem_Free (baseBuf);
	if (normBuf) Mem_Free (normBuf);
	if (emisBuf) Mem_Free (emisBuf);
	if (glossBuf) Mem_Free (glossBuf);

	if (emissR > 0.0f || emissG > 0.0f || emissB > 0.0f)
	{
		glt->rtemissive = true;
		glt->rtemissivecolor[0] = emissR / (npix * 255.0f);
		glt->rtemissivecolor[1] = emissG / (npix * 255.0f);
		glt->rtemissivecolor[2] = emissB / (npix * 255.0f);
		glt->rtemissivemean = (float)(emissMean / npix);

		// Apply the material's light_brightness multiplier so emissive lights
		// (is_light: true without light_color, e.g. flame skins) scale exactly
		// like curated light_color lights do (see the has_light_color path
		// above). rtemissivecolor is only consumed by light generation
		// (r_world.c / r_alias.c), so this does not brighten the surface
		// emissive itself.
		if (mat->light_brightness != 1.0f)
			ModifyColorValue (glt->rtemissivecolor, mat->light_brightness);
	}

	// explicit "is_light" flag: this texture may generate static emissive
	// area lights (gated on rtislight in r_world.c)
	glt->rtislight = mat->is_light;

	// 4.6 debug: report which material was applied and the average emissive
	extern cvar_t rt_mat_debug;
	if (CVAR_TO_BOOL (rt_mat_debug))
	{
		double emSum = 0.0;
		for (int i = 0; i < npix; i++)
		{
			emSum += rme[i * 4 + 2];
		}
		Con_Printf ("RT: applied material '%s' (glt='%s') base=%s norm=%s emis=%s gloss=%s avg_emis=%.1f/255 light_brightness=%.3f is_light=%d rtemissive=%d rtemissivecolor=(%.4f, %.4f, %.4f)\n",
		            mat->name, glt->name,
		            mat->filename_base[0] ? mat->filename_base : "-",
		            mat->filename_normals[0] ? mat->filename_normals : "-",
		            mat->filename_emissive[0] ? mat->filename_emissive : "-",
		            mat->filename_gloss[0] ? mat->filename_gloss : "-",
		            npix > 0 ? emSum / npix : 0.0,
		            mat->light_brightness,
		            glt->rtislight ? 1 : 0,
		            glt->rtemissive ? 1 : 0,
		            glt->rtemissivecolor[0], glt->rtemissivecolor[1], glt->rtemissivecolor[2]);
	}

	RgMaterialCreateInfo info = {
		.flags = TexMgr_GetRtFlags (glt),
		.size = {tw, th},
		.textures =
			{
				.pDataAlbedoAlpha = albedo,
				.pDataRoughnessMetallicEmission = rme,
				.pDataNormal = normal,
			},
		.pRelativePath = glt->rtname,
		.filter = TexMgr_GetFilterMode (glt),
		.addressModeU = RG_SAMPLER_ADDRESS_MODE_REPEAT,
		.addressModeV = RG_SAMPLER_ADDRESS_MODE_REPEAT,
	};

	RgMaterial oldMaterial = glt->rtmaterial;
	RgMaterial newMaterial = RG_NULL_HANDLE;
	SDL_LockMutex (rtspecial_mutex);
	RgResult r = rgCreateMaterial (vulkan_globals.instance, &info, &newMaterial);
	SDL_UnlockMutex (rtspecial_mutex);
	RG_CHECK (r);

	if (oldMaterial)
		rgDestroyMaterial (vulkan_globals.instance, oldMaterial);
	glt->rtmaterial = newMaterial;

	Mem_Free (albedo);
	Mem_Free (rme);
	Mem_Free (normal);

	return true;
}

/*
================
TexMgr_LoadImage8 -- handles 8bit source data, then passes it to LoadImage32
================
*/
static void TexMgr_LoadImage8 (gltexture_t *glt, byte *data)
{
	GL_DeleteTexture (glt);

	extern cvar_t gl_fullbrights;
	unsigned int *usepal;
	int           i;

	// HACK HACK HACK -- taken from tomazquake
	if (strstr (glt->name, "shot1sid") && glt->width == 32 && glt->height == 32 && CRC_Block (data, 1024) == 65393)
	{
		// This texture in b_shell1.bsp has some of the first 32 pixels painted white.
		// They are invisible in software, but look really ugly in GL. So we just copy
		// 32 pixels from the bottom to make it look nice.
		memcpy (data, data + 32 * 31, 32);
	}

	// detect false alpha cases
	if (glt->flags & TEXPREF_ALPHA && !(glt->flags & TEXPREF_CONCHARS))
	{
		for (i = 0; i < (int)(glt->width * glt->height); i++)
			if (data[i] == 255) // transparent index
				break;
		if (i == (int)(glt->width * glt->height))
			glt->flags -= TEXPREF_ALPHA;
	}

	// choose palette and padbyte
	if (glt->flags & TEXPREF_CONCHARS)
	{
		usepal = d_8to24table_conchars;
	}
#if !RT_RENDERER
	else if (glt->flags & TEXPREF_NOBRIGHT && gl_fullbrights.value)
	{
		if (glt->flags & TEXPREF_ALPHA)
			usepal = d_8to24table_nobright_fence;
		else
			usepal = d_8to24table_nobright;
	}
#endif
	else
	{
		usepal = d_8to24table;
	}

	// convert to 32bit
	unsigned *converted;
	TEMP_ALLOC (unsigned, converted, glt->width * glt->height);
	TexMgr_8to32 (data, converted, glt->width * glt->height, usepal);

	// fix edges
	if (glt->flags & TEXPREF_ALPHA)
		TexMgr_AlphaEdgeFix ((byte *)converted, glt->width, glt->height);

	// upload it
	TexMgr_LoadImage32 (glt, (unsigned *)converted);

	TEMP_FREE (converted);
}

/*
================
TexMgr_LoadLightmap -- handles lightmap data
================
*/
static void TexMgr_LoadLightmap (gltexture_t *glt, byte *data)
{
	TexMgr_LoadImage32 (glt, (unsigned *)data);
}

/*
================
TexMgr_LoadImage -- the one entry point for loading all textures
================
*/
gltexture_t *TexMgr_LoadImage (
	const char *rtname,
	qmodel_t *owner, const char *name, int width, int height, enum srcformat format, byte *data, const char *source_file, src_offset_t source_offset,
	unsigned flags)
{
	unsigned short crc = 0;
	gltexture_t   *glt;

	if (isDedicated)
		return NULL;

	// cache check
	if (flags & TEXPREF_OVERWRITE)
		switch (format)
		{
		case SRC_INDEXED:
			crc = CRC_Block (data, width * height);
			break;
		case SRC_LIGHTMAP:
			crc = CRC_Block (data, width * height * LIGHTMAP_BYTES);
			break;
		case SRC_RGBA:
			crc = CRC_Block (data, width * height * 4);
			break;
		default: /* not reachable but avoids compiler warnings */
			crc = 0;
		}
	if ((flags & TEXPREF_OVERWRITE) && (glt = TexMgr_FindTexture (owner, name)))
	{
		if (glt->source_crc == crc)
			return glt;
	}
	else
		glt = TexMgr_NewTexture ();

	// copy data
	glt->owner = owner;
	q_strlcpy (glt->name, name, sizeof (glt->name));
	glt->width = width;
	glt->height = height;
	glt->flags = flags;
	glt->shirt = -1;
	glt->pants = -1;
	q_strlcpy (glt->source_file, source_file, sizeof (glt->source_file));
	glt->source_offset = source_offset;
	glt->source_format = format;
	glt->source_width = width;
	glt->source_height = height;
	glt->source_crc = crc;

	if (rtname)
	{
	    q_strlcpy (glt->rtname, rtname, sizeof (glt->rtname));
	}
	else
	{
		glt->rtname[0] = '\0';
	}

	// TexMgr_NewTexture reuses pooled gltexture_t objects, so any RT flags from
	// a previous load must be cleared here before TexMgr_ApplyMaterialFromMat
	// (called from TexMgr_LoadImage8/32 below) re-populates them. Textures
	// without a material otherwise retain stale values.
	glt->rtlightcolor[0] = glt->rtlightcolor[1] = glt->rtlightcolor[2] = 0.0f;
	glt->rthaslightcolor = false;
	glt->rtupoffset = 0.0f;
	glt->rtmirror = false;
	glt->rtexactnormals = false;
	glt->rtforcerasterize = false;
	glt->rtemissive = false;
	glt->rtemissivecolor[0] = glt->rtemissivecolor[1] = glt->rtemissivecolor[2] = 0.0f;
	glt->rtemissivemean = 0.0f;
	glt->rtemissivetex = false;
	glt->rtislight = false;

	// upload it
	switch (glt->source_format)
	{
	case SRC_INDEXED:
		TexMgr_LoadImage8 (glt, data);
		break;
	case SRC_LIGHTMAP:
		TexMgr_LoadLightmap (glt, data);
		break;
	case SRC_RGBA:
	case SRC_SURF_INDICES:
		TexMgr_LoadImage32 (glt, (unsigned *)data);
		break;
	}

	return glt;
}

/*
================================================================================

    COLORMAPPING AND TEXTURE RELOADING

================================================================================
*/

/*
================
TexMgr_ReloadImage -- reloads a texture, and colormaps it if needed
================
*/
void TexMgr_ReloadImage (gltexture_t *glt, int shirt, int pants)
{
	byte  translation[256];
	byte *src, *dst, *data = NULL, *allocated = NULL, *translated = NULL;
	int   size, i;
	//
	// get source data
	//

	if (glt->source_file[0] && glt->source_offset)
	{
		// lump inside file
		FILE *f;
		COM_FOpenFile (glt->source_file, &f, NULL);
		if (!f)
			goto invalid;
		fseek (f, glt->source_offset, SEEK_CUR);
		size = glt->source_width * glt->source_height;
		/* should be SRC_INDEXED, but no harm being paranoid:  */
		if (glt->source_format == SRC_RGBA)
		{
			size *= 4;
		}
		else if (glt->source_format == SRC_LIGHTMAP)
		{
			size *= LIGHTMAP_BYTES;
		}
		allocated = data = (byte *)Mem_Alloc (size);
		if (fread (data, 1, size, f) != size)
			goto invalid;
		fclose (f);
	}
	else if (glt->source_file[0] && !glt->source_offset)
	{
		allocated = data = Image_LoadImage (glt->source_file, (int *)&glt->source_width, (int *)&glt->source_height); // simple file
	}
	else if (!glt->source_file[0] && glt->source_offset)
	{
		data = (byte *)glt->source_offset; // image in memory
	}
	if (!data)
	{
	invalid:
		Con_Printf ("TexMgr_ReloadImage: invalid source for %s\n", glt->name);
		return;
	}

	glt->width = glt->source_width;
	glt->height = glt->source_height;
	//
	// apply shirt and pants colors
	//
	// if shirt and pants are -1,-1, use existing shirt and pants colors
	// if existing shirt and pants colors are -1,-1, don't bother colormapping
	if (shirt > -1 && pants > -1)
	{
		if (glt->source_format == SRC_INDEXED)
		{
			glt->shirt = shirt;
			glt->pants = pants;
		}
		else
			Con_Printf ("TexMgr_ReloadImage: can't colormap a non SRC_INDEXED texture: %s\n", glt->name);
	}
	if (glt->shirt > -1 && glt->pants > -1)
	{
		// create new translation table
		for (i = 0; i < 256; i++)
			translation[i] = i;

		shirt = glt->shirt * 16;
		if (shirt < 128)
		{
			for (i = 0; i < 16; i++)
				translation[TOP_RANGE + i] = shirt + i;
		}
		else
		{
			for (i = 0; i < 16; i++)
				translation[TOP_RANGE + i] = shirt + 15 - i;
		}

		pants = glt->pants * 16;
		if (pants < 128)
		{
			for (i = 0; i < 16; i++)
				translation[BOTTOM_RANGE + i] = pants + i;
		}
		else
		{
			for (i = 0; i < 16; i++)
				translation[BOTTOM_RANGE + i] = pants + 15 - i;
		}

		// translate texture
		size = glt->width * glt->height;
		dst = translated = (byte *)Mem_Alloc (size);
		src = data;

		for (i = 0; i < size; i++)
			*dst++ = translation[*src++];

		data = translated;
	}
	//
	// upload it
	//
	switch (glt->source_format)
	{
	case SRC_INDEXED:
		TexMgr_LoadImage8 (glt, data);
		break;
	case SRC_LIGHTMAP:
		TexMgr_LoadLightmap (glt, data);
		break;
	case SRC_RGBA:
	case SRC_SURF_INDICES:
		TexMgr_LoadImage32 (glt, (unsigned *)data);
		break;
	}

	Mem_Free (translated);
	Mem_Free (allocated);
}

/*
================
TexMgr_ReloadNobrightImages -- reloads all texture that were loaded with the nobright palette.  called when gl_fullbrights changes
================
*/
void TexMgr_ReloadNobrightImages (void)
{
#if !RT_RENDERER
	gltexture_t *glt;

	for (glt = active_gltextures; glt; glt = glt->next)
		if (glt->flags & TEXPREF_NOBRIGHT)
			TexMgr_ReloadImage (glt, -1, -1);
#endif
}

/*
================================================================================

    TEXTURE BINDING / TEXTURE UNIT SWITCHING

================================================================================
*/

/*
================
GL_DeleteTexture
================
*/
static void GL_DeleteTexture (gltexture_t *texture)
{
	SDL_LockMutex (texmgr_mutex);

	if (texture->rtmaterial != RG_NO_MATERIAL)
	{
		RgResult r = rgDestroyMaterial (vulkan_globals.instance, texture->rtmaterial);
		RG_CHECK (r);

		texture->rtmaterial = RG_NO_MATERIAL;
	}

	SDL_UnlockMutex (texmgr_mutex);
}
