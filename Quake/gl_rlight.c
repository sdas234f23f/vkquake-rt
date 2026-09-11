/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2010-2014 QuakeSpasm developers

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
// r_light.c

#include "quakedef.h"

int r_dlightframecount;

extern cvar_t r_flatlightstyles; // johnfitz
extern cvar_t r_lerplightstyles;
extern cvar_t r_gpulightmapupdate;

extern SDL_mutex *lightcache_mutex;

/*
==================
R_AnimateLight
==================
*/
void R_AnimateLight (void)
{
	int    i, j, k, n;
	double f;

	//
	// light animations
	// 'm' is normal light, 'a' is no light, 'z' is double bright
	i = f = cl.time * 10;
	for (j = 0; j < MAX_LIGHTSTYLES; j++)
	{
		if (!cl_lightstyle[j].length)
		{
			d_lightstylevalue[j] = 256;
			continue;
		}
		// johnfitz -- r_flatlightstyles
		if (r_flatlightstyles.value == 2)
			k = n = cl_lightstyle[j].peak - 'a';
		else if (r_flatlightstyles.value == 1)
			k = n = cl_lightstyle[j].average - 'a';
		else
		{
			k = cl_lightstyle[j].map[i % cl_lightstyle[j].length] - 'a';
			n = cl_lightstyle[j].map[(i + 1) % cl_lightstyle[j].length] - 'a';
		}
		if (!r_gpulightmapupdate.value || !r_lerplightstyles.value || (r_lerplightstyles.value < 2 && abs (n - k) >= ('m' - 'a') / 2))
			n = k;
		d_lightstylevalue[j] = (k + (n - k) * (f - i)) * 22;
		// johnfitz
	}
}

/*
=============================================================================

DYNAMIC LIGHTS

=============================================================================
*/

/*
=============
R_MarkLights -- johnfitz -- rewritten to use LordHavoc's lighting speedup
=============
*/
void R_MarkLights (dlight_t *light, int num, mnode_t *node)
{
	mplane_t    *splitplane;
	msurface_t  *surf;
	vec3_t       impact;
	float        dist, l, maxdist;
	unsigned int i;
	int          j, s, t;

start:

	if (node->contents < 0)
		return;

	splitplane = node->plane;
	if (splitplane->type < 3)
		dist = light->origin[splitplane->type] - splitplane->dist;
	else
		dist = DotProduct (light->origin, splitplane->normal) - splitplane->dist;

	if (dist > light->radius)
	{
		node = node->children[0];
		goto start;
	}
	if (dist < -light->radius)
	{
		node = node->children[1];
		goto start;
	}

	maxdist = light->radius * light->radius;
	// mark the polygons
	surf = cl.worldmodel->surfaces + node->firstsurface;
	for (i = 0; i < node->numsurfaces; i++, surf++)
	{
		for (j = 0; j < 3; j++)
			impact[j] = light->origin[j] - surf->plane->normal[j] * dist;
		// clamp center of light to corner and check brightness
		l = DotProduct (impact, surf->texinfo->vecs[0]) + surf->texinfo->vecs[0][3] - surf->texturemins[0];
		s = l + 0.5;
		if (s < 0)
			s = 0;
		else if (s > surf->extents[0])
			s = surf->extents[0];
		s = l - s;
		l = DotProduct (impact, surf->texinfo->vecs[1]) + surf->texinfo->vecs[1][3] - surf->texturemins[1];
		t = l + 0.5;
		if (t < 0)
			t = 0;
		else if (t > surf->extents[1])
			t = surf->extents[1];
		t = l - t;
		// compare to minimum light
		if ((s * s + t * t + dist * dist) < maxdist)
		{
			if (surf->dlightframe != r_dlightframecount) // not dynamic until now
			{
				surf->dlightbits[num >> 5] = 1U << (num & 31);
				surf->dlightframe = r_dlightframecount;
			}
			else // already dynamic
				surf->dlightbits[num >> 5] |= 1U << (num & 31);
		}
	}

	if (node->children[0]->contents >= 0)
		R_MarkLights (light, num, node->children[0]);
	if (node->children[1]->contents >= 0)
		R_MarkLights (light, num, node->children[1]);
}

/*
=============
R_PushDlights
=============
*/
void R_PushDlights (void)
{
	int       i;
	dlight_t *l;

	r_dlightframecount = r_framecount + 1; // because the count hasn't
	                                       //  advanced yet for this frame
	l = cl_dlights;

	for (i = 0; i < MAX_DLIGHTS; i++, l++)
	{
		if (l->die < cl.time || !l->radius)
			continue;
		R_MarkLights (l, i, cl.worldmodel->nodes);
	}
}

/*
=============================================================================

LIGHT SAMPLING

=============================================================================
*/

static void InterpolateLightmap (vec3_t color, msurface_t *surf, int ds, int dt)
{
	byte *lightmap;
	int   maps, line3, dsfrac = ds & 15, dtfrac = dt & 15, r00 = 0, g00 = 0, b00 = 0, r01 = 0, g01 = 0, b01 = 0, r10 = 0, g10 = 0, b10 = 0, r11 = 0, g11 = 0,
					 b11 = 0;
	int scale;
	line3 = ((surf->extents[0] >> 4) + 1) * 3;

	lightmap = surf->samples + ((dt >> 4) * ((surf->extents[0] >> 4) + 1) + (ds >> 4)) * 3; // LordHavoc: *3 for color

	for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
	{
		scale = d_lightstylevalue[surf->styles[maps]];
		r00 += lightmap[0] * scale;
		g00 += lightmap[1] * scale;
		b00 += lightmap[2] * scale;
		r01 += lightmap[3] * scale;
		g01 += lightmap[4] * scale;
		b01 += lightmap[5] * scale;
		r10 += lightmap[line3 + 0] * scale;
		g10 += lightmap[line3 + 1] * scale;
		b10 += lightmap[line3 + 2] * scale;
		r11 += lightmap[line3 + 3] * scale;
		g11 += lightmap[line3 + 4] * scale;
		b11 += lightmap[line3 + 5] * scale;
		lightmap += ((surf->extents[0] >> 4) + 1) * ((surf->extents[1] >> 4) + 1) * 3; // LordHavoc: *3 for colored lighting
	}

	color[0] = ((((((((r11 - r10) * dsfrac) >> 4) + r10) - ((((r01 - r00) * dsfrac) >> 4) + r00)) * dtfrac) >> 4) + ((((r01 - r00) * dsfrac) >> 4) + r00)) *
	           (1.f / 256.f);
	color[1] = ((((((((g11 - g10) * dsfrac) >> 4) + g10) - ((((g01 - g00) * dsfrac) >> 4) + g00)) * dtfrac) >> 4) + ((((g01 - g00) * dsfrac) >> 4) + g00)) *
	           (1.f / 256.f);
	color[2] = ((((((((b11 - b10) * dsfrac) >> 4) + b10) - ((((b01 - b00) * dsfrac) >> 4) + b00)) * dtfrac) >> 4) + ((((b01 - b00) * dsfrac) >> 4) + b00)) *
	           (1.f / 256.f);
}

/*
=============
RecursiveLightPoint -- johnfitz -- replaced entire function for lit support via lordhavoc
=============
*/
int RecursiveLightPoint (lightcache_t *cache, mnode_t *node, vec3_t rayorg, vec3_t start, vec3_t end, float *maxdist)
{
	float  front, back, frac;
	vec3_t mid;

loc0:
	if (node->contents < 0)
		return false; // didn't hit anything

	// calculate mid point
	if (node->plane->type < 3)
	{
		front = start[node->plane->type] - node->plane->dist;
		back = end[node->plane->type] - node->plane->dist;
	}
	else
	{
		front = DotProduct (start, node->plane->normal) - node->plane->dist;
		back = DotProduct (end, node->plane->normal) - node->plane->dist;
	}

	// LordHavoc: optimized recursion
	if ((back < 0) == (front < 0))
	//		return RecursiveLightPoint (cache, node->children[front < 0], rayorg, start, end, maxdist);
	{
		node = node->children[front < 0];
		goto loc0;
	}

	frac = front / (front - back);
	mid[0] = start[0] + (end[0] - start[0]) * frac;
	mid[1] = start[1] + (end[1] - start[1]) * frac;
	mid[2] = start[2] + (end[2] - start[2]) * frac;

	// go down front side
	if (RecursiveLightPoint (cache, node->children[front < 0], rayorg, start, mid, maxdist))
		return true; // hit something
	else
	{
		unsigned int i;
		int          ds, dt;
		msurface_t  *surf;

		surf = cl.worldmodel->surfaces + node->firstsurface;
		for (i = 0; i < node->numsurfaces; i++, surf++)
		{
			float  sfront, sback, dist;
			vec3_t raydelta;

			if (surf->flags & SURF_DRAWTILED)
				continue; // no lightmaps

			// ericw -- added double casts to force 64-bit precision.
			// Without them the zombie at the start of jam3_ericw.bsp was
			// incorrectly being lit up in SSE builds.
			ds = (int)((double)DoublePrecisionDotProduct (mid, surf->texinfo->vecs[0]) + surf->texinfo->vecs[0][3]);
			dt = (int)((double)DoublePrecisionDotProduct (mid, surf->texinfo->vecs[1]) + surf->texinfo->vecs[1][3]);

			if (ds < surf->texturemins[0] || dt < surf->texturemins[1])
				continue;

			ds -= surf->texturemins[0];
			dt -= surf->texturemins[1];

			if (ds > surf->extents[0] || dt > surf->extents[1])
				continue;

			if (surf->plane->type < 3)
			{
				sfront = rayorg[surf->plane->type] - surf->plane->dist;
				sback = end[surf->plane->type] - surf->plane->dist;
			}
			else
			{
				sfront = DotProduct (rayorg, surf->plane->normal) - surf->plane->dist;
				sback = DotProduct (end, surf->plane->normal) - surf->plane->dist;
			}
			VectorSubtract (end, rayorg, raydelta);
			dist = sfront / (sfront - sback) * VectorLength (raydelta);

			if (!surf->samples)
			{
				// We hit a surface that is flagged as lightmapped, but doesn't have actual lightmap info.
				// Instead of just returning black, we'll keep looking for nearby surfaces that do have valid samples.
				// This fixes occasional pitch-black models in otherwise well-lit areas in DOTM (e.g. mge1m1, mge4m1)
				// caused by overlapping surfaces with mixed lighting data.
				const float nearby = 8.f;
				dist += nearby;
				*maxdist = q_min (*maxdist, dist);
				continue;
			}

			if (dist < *maxdist)
			{
				cache->surfidx = surf - cl.worldmodel->surfaces + 1;
				cache->ds = ds;
				cache->dt = dt;
			}
			else
			{
				cache->surfidx = -1;
			}

			return true; // success
		}

		// go down back side
		return RecursiveLightPoint (cache, node->children[front >= 0], rayorg, mid, end, maxdist);
	}
}

/*
=============
R_LightPoint -- johnfitz -- replaced entire function for lit support via lordhavoc
=============
*/
int R_LightPoint (vec3_t p, lightcache_t *cache, vec3_t *lightcolor)
{
	vec3_t end;
	float  maxdist = 8192.f; // johnfitz -- was 2048

	if (!cl.worldmodel->lightdata)
	{
		(*lightcolor)[0] = (*lightcolor)[1] = (*lightcolor)[2] = 255;
		return 255;
	}

	end[0] = p[0];
	end[1] = p[1];
	end[2] = p[2] - maxdist;

	(*lightcolor)[0] = (*lightcolor)[1] = (*lightcolor)[2] = 0;

	SDL_mutex *mtx = cache->mutex ? cache->mutex : lightcache_mutex;
	SDL_LockMutex (mtx);
	if (!cache || cache->surfidx <= 0 // no cache or pitch black
	    || cache->surfidx > cl.worldmodel->numsurfaces || fabsf (cache->pos[0] - p[0]) >= 1.f || fabsf (cache->pos[1] - p[1]) >= 1.f ||
	    fabsf (cache->pos[2] - p[2]) >= 1.f)
	{
		cache->surfidx = 0;
		VectorCopy (p, cache->pos);
		RecursiveLightPoint (cache, cl.worldmodel->nodes, p, p, end, &maxdist);
	}

	if (cache && cache->surfidx > 0)
		InterpolateLightmap (*lightcolor, cl.worldmodel->surfaces + cache->surfidx - 1, cache->ds, cache->dt);
	SDL_UnlockMutex (mtx);

	return (((*lightcolor)[0] + (*lightcolor)[1] + (*lightcolor)[2]) * (1.0f / 3.0f));
}



extern cvar_t rt_elight_normaliz, rt_elight_default, rt_elight_default_mdl, rt_elight_radius, rt_elight_threshold;
extern cvar_t rt_truelight;
extern cvar_t rt_materials_only;
extern cvar_t rt_poi_trigger, rt_poi_func, rt_poi_weapon, rt_poi_pwrup, rt_poi_armor, rt_poi_key, rt_poi_health, rt_poi_ammo;
extern cvar_t rt_poi_distthresh, rt_poi_distthresh_super;
extern cvar_t rt_light_reach;
extern cvar_t rt_light_report_filter;


static qboolean StartsWith (const char *val, const char *begin)
{
	return strncmp (val, begin, strlen (begin)) == 0;
}


// look https://www.gamers.org/dEngine/quake/QDP/qmapspec.html#2.3.1
// for classnames

static qboolean IsClassname_Light (const char *classname)
{
	return StartsWith (classname, "light");
}

static qboolean IsClassname_LightWithModel (const char *classname)
{
	// For example,
	//    "light_fluoro"
	//    "light_fluorospark"
	//    "light_globe"
	//    "light_torch_small_walltorch"
	//    "light_flame_small_yellow"
	//    "light_flame_large_yellow"
	//    "light_flame_small_white"
	// but not just "light"
	
	return StartsWith (classname, "light_");
}

static qboolean IsClassname_Offsetted (const char *classname)
{
	// to prevent light source being inside the flame model
	return strcmp (classname, "light_torch_small_walltorch") == 0;
}

static qboolean IsClassname_PointOfInterest (const char *classname, qboolean *out_superimportant)
{
	if (CVAR_TO_BOOL (rt_poi_trigger))
	{
		if (StartsWith (classname, "trigger") || strcmp (classname, "info_teleport_destination") == 0)
		{
			*out_superimportant = true;
			return true;
		}
	}

	if (CVAR_TO_BOOL (rt_poi_func))
	{
		if (StartsWith (classname, "func"))
		{
			*out_superimportant = true;
			return true;
		}
	}

	if (CVAR_TO_BOOL (rt_poi_weapon))
	{
		if (strcmp (classname, "item_weapon") == 0 ||
			StartsWith(classname, "weapon"))
		{
			return true;
		}
	}

	if (StartsWith (classname, "item"))
	{
		if (CVAR_TO_BOOL (rt_poi_pwrup))
		{
			if (StartsWith (classname, "item_artifact"))
			{
				return true;
			}
		}

		if (CVAR_TO_BOOL (rt_poi_armor))
		{
			if (StartsWith (classname, "item_armor"))
			{
				return true;
			}
		}

		if (CVAR_TO_BOOL (rt_poi_key))
		{
			if (strcmp (classname, "item_sigil") == 0 || 
				StartsWith (classname, "item_key"))
			{
				*out_superimportant = true;
				return true;
			}
		}

		if (CVAR_TO_BOOL (rt_poi_ammo))
		{
			if (strcmp (classname, "item_cells") == 0 || 
				strcmp (classname, "item_rockets") == 0 || 
				strcmp (classname, "item_shells") == 0 ||
			    strcmp (classname, "item_spikes") == 0)
			{
				return true;
			}
		}

		if (CVAR_TO_BOOL (rt_poi_health))
		{
			if (strcmp (classname, "item_health") == 0)
			{
				return true;
			}
		}
	}

	return false;
}



typedef struct rt_poi_s
{
	vec3_t origin;
	qboolean is_super_imporant;
} rt_poi_t;

rt_poi_t *rt_poi = NULL;
int       rt_poi_count = 0;
int       rt_poi_allocated = 0;

static void RT_ParsePointsOfInterest ()
{
	rt_poi_count = 0;

    char key[128], value[4096];

    if (!cl.worldmodel)
	{
		return;
	}

	const char* data = cl.worldmodel->entities;
	if (!data)
	{
		return;
	}
	
	rt_poi_t cur_values = {0};
	int      cur_state = 0;

    #define CUR_STRUCT_STARTED 1
    #define CUR_IS_POI         2
    #define CUR_FOUND_ORIGIN   4
	    
	while (1)
	{
		data = COM_Parse (data);
		if (!data)
			return; // error

	    if (com_token[0] == '{')
	    {
			memset (&cur_values, 0, sizeof (cur_values));
			cur_state = CUR_STRUCT_STARTED;
            continue;
	    }
	    else if (com_token[0] == '}')
		{
			if ((cur_state & CUR_STRUCT_STARTED) &&
			    (cur_state & CUR_IS_POI) &&
			    (cur_state & CUR_FOUND_ORIGIN))
			{
				if (rt_poi_count >= rt_poi_allocated)
				{
					rt_poi_allocated += 256;
					rt_poi = Mem_Realloc (rt_poi, sizeof (rt_poi_t) * rt_poi_allocated);
				}

				rt_poi[rt_poi_count] = cur_values;
				rt_poi_count++;
			}

			cur_state = 0; // end of struct
			continue;
		}

		if (com_token[0] == '_')
			q_strlcpy (key, com_token + 1, sizeof (key));
		else
			q_strlcpy (key, com_token, sizeof (key));
		while (key[0] && key[strlen (key) - 1] == ' ') // remove trailing spaces
			key[strlen (key) - 1] = 0;
		data = COM_Parse (data);
		if (!data)
			return; // error
		q_strlcpy (value, com_token, sizeof (value));

		
		if (strcmp (key, "classname") == 0)
		{
			qboolean is_super = 0;

			if (IsClassname_PointOfInterest (value, &is_super))
			{
				cur_state |= CUR_IS_POI;
				cur_values.is_super_imporant = is_super;
			}
		}
		else if (strcmp (key, "origin") == 0)
		{
			vec3_t tmpvec;
			int    components = sscanf (value, "%f %f %f", &tmpvec[0], &tmpvec[1], &tmpvec[2]);

			if (components == 3)
			{
				cur_values.origin[0] = tmpvec[0];
				cur_values.origin[1] = tmpvec[1];
				cur_values.origin[2] = tmpvec[2];
				cur_state |= CUR_FOUND_ORIGIN;
			}
		}
	}

}

static qboolean IsAroundPOI (vec3_t origin)
{
	float threshold = METRIC_TO_QUAKEUNIT (CVAR_TO_FLOAT (rt_poi_distthresh));
	float threshold_loose = METRIC_TO_QUAKEUNIT (CVAR_TO_FLOAT (rt_poi_distthresh_super));

	threshold *= threshold;
	threshold_loose *= threshold_loose;

	for (int i = 0; i < rt_poi_count;i++)
	{
		const rt_poi_t *src = &rt_poi[i];

		vec3_t v;
		VectorSubtract (src->origin, origin, v);

		float distsq_thresh = src->is_super_imporant ? threshold_loose : threshold;

		if (DotProduct (v, v) < distsq_thresh)
		{
			return true;
		}
	}

	return false;
}



typedef struct rt_elight_s
{
	int      state;
	vec3_t   origin;
	float    intensity;
	int      lightstyle;
	qboolean is_around_poi;
} rt_elight_t;

rt_elight_t *rt_elights = NULL;
int          rt_elights_count = 0;
int          rt_elights_allocated = 0;

// Parse worldmodel->entities, to find static lights
void RT_ParseElights ()
{
	rt_elights_count = 0;

	RT_ParsePointsOfInterest ();


	char key[128], value[4096];

    if (!cl.worldmodel)
	{
		return;
	}

	const char* data = cl.worldmodel->entities;
	if (!data)
	{
		return;
	}
	
	rt_elight_t struct_values = {0};

    #define STRUCT_STATE_STRUCT_STARTED       1
    #define STRUCT_STATE_FOUND_LIGHTCLASSNAME 2
    #define STRUCT_STATE_FOUND_ORIGIN         4
    #define STRUCT_STATE_FOUND_INTENSITY      8
    #define STRUCT_STATE_FOUND_WITH_MODEL     16
    #define STRUCT_STATE_FOUND_LIGHTSTYLE     32
    #define STRUCT_STATE_FOUND_APPLY_OFFSET   64
	
	while (1)
	{
		data = COM_Parse (data);
		if (!data)
			return; // error

	    if (com_token[0] == '{')
	    {
			memset (&struct_values, 0, sizeof (struct_values));
			struct_values.state = STRUCT_STATE_STRUCT_STARTED;
            continue;
	    }
	    else if (com_token[0] == '}')
		{
			if ((struct_values.state & STRUCT_STATE_STRUCT_STARTED) &&
			    (struct_values.state & STRUCT_STATE_FOUND_LIGHTCLASSNAME) &&
			    (struct_values.state & STRUCT_STATE_FOUND_ORIGIN))
			{
				struct_values.is_around_poi = IsAroundPOI (struct_values.origin);


				if (rt_elights_count >= rt_elights_allocated)
				{
					rt_elights_allocated += 256;
					rt_elights = Mem_Realloc (rt_elights, sizeof (rt_elight_t) * rt_elights_allocated);
				}
				rt_elights[rt_elights_count] = struct_values;
				rt_elights_count++;
			}

			struct_values.state = 0; // end of struct
			continue;
		}

		if (com_token[0] == '_')
			q_strlcpy (key, com_token + 1, sizeof (key));
		else
			q_strlcpy (key, com_token, sizeof (key));
		while (key[0] && key[strlen (key) - 1] == ' ') // remove trailing spaces
			key[strlen (key) - 1] = 0;
		data = COM_Parse (data);
		if (!data)
			return; // error
		q_strlcpy (value, com_token, sizeof (value));

		
		if (strcmp (key, "classname") == 0)
		{
			if (IsClassname_Light (value))
			{
				struct_values.state |= STRUCT_STATE_FOUND_LIGHTCLASSNAME;
			}

			if (IsClassname_LightWithModel (value))
			{
				struct_values.state |= STRUCT_STATE_FOUND_WITH_MODEL;

				if (IsClassname_Offsetted (value))
				{
					struct_values.state |= STRUCT_STATE_FOUND_APPLY_OFFSET;
				}
			}
		}
		else if (strcmp (key, "origin") == 0)
		{
			vec3_t tmpvec;
			int    components = sscanf (value, "%f %f %f", &tmpvec[0], &tmpvec[1], &tmpvec[2]);

			if (components == 3)
			{
				struct_values.origin[0] = tmpvec[0];
				struct_values.origin[1] = tmpvec[1];
				struct_values.origin[2] = tmpvec[2];
				struct_values.state |= STRUCT_STATE_FOUND_ORIGIN;
			}
		}
		else if (strcmp (key, "light") == 0)
		{
			float tmpval = strtof(value, NULL);

		    if (tmpval > 0.0f)
			{
				struct_values.intensity = tmpval;
				struct_values.state |= STRUCT_STATE_FOUND_INTENSITY;
			}
		}
		else if (strcmp (key, "style") == 0)
		{
			int tmpval = strtol (value, NULL, 10);

			if (tmpval >= 0 && tmpval < MAX_LIGHTSTYLES)
			{
				struct_values.lightstyle = tmpval;
				struct_values.state |= STRUCT_STATE_FOUND_LIGHTSTYLE;
			}
		}
	}
}

// ============================================================================
// Strict light-source modes
// ----------------------------------------------------------------------------
// The two strict-mode cvars (rt_materials_only / rt_truelight) tell the light
// uploaders which categories of light may exist in a frame. Everything funnels
// through RT_AllowFakeLights() below so the condition lives in one place:
//
//                         rt_truelight  0   1   2   |  rt_materials_only 1
//   textured-area lights (luma / light_color)      on   on   on   |  on
//   sky / sun                                      on   on   on   |  off
//   flashlight                                     on   on   on   |  off
//   classic dlights (muzzle flash / explosions)    on   on   off  |  off
//   world light_color spheres (removed)            -    -    -    |  -
//   model/sprite light_color spheres               on   on   off  |  off
//   legacy entity "light" points                   on   off  off  |  off
//
// rt_truelight 0 is the legacy "everything glows" look, 1 is the physical
// default (luma/emissive materials + real dynamic events like flashes), and 2
// restricts to physically-plausible light sources only (no floating fake
// points); rt_materials_only additionally drops the sky and the flashlight.
// ============================================================================
qboolean RT_AllowFakeLights (void)
{
	// Fake lights = classic point-light approximations that "hang in the air"
	// (dlights, model/sprite light_color spheres). They are suppressed in
	// materials-only mode and at rt_truelight 2.
	return !CVAR_TO_BOOL (rt_materials_only) && CVAR_TO_FLOAT (rt_truelight) < 2;
}

void RT_UploadAllElights ()
{
	// Legacy entity lights (map "light" entities that hang in the air) are not
	// physically-plausible sources: they exist only in legacy rt_truelight 0
	// mode. Both strict modes (rt_truelight > 0, materials_only) skip them.
	if (CVAR_TO_FLOAT (rt_truelight) > 0 || CVAR_TO_BOOL (rt_materials_only))
	{
		return;
	}

	if (CVAR_TO_FLOAT (rt_elight_normaliz) < 0.5f)
	{
		return;
	}

	for (int i = 0; i < rt_elights_count; i++)
	{
		const rt_elight_t *src = &rt_elights[i];

		assert (src->state & STRUCT_STATE_STRUCT_STARTED);
		assert (src->state & STRUCT_STATE_FOUND_LIGHTCLASSNAME);
		assert (src->state & STRUCT_STATE_FOUND_ORIGIN);

		float quake_intensity;
		if (src->state & STRUCT_STATE_FOUND_INTENSITY)
		{
			quake_intensity = src->intensity;
		}
		else
		{
			quake_intensity = src->state & STRUCT_STATE_FOUND_WITH_MODEL ? CVAR_TO_FLOAT (rt_elight_default_mdl) : CVAR_TO_FLOAT (rt_elight_default);
		}

		qboolean accept = 
			quake_intensity >= CVAR_TO_FLOAT (rt_elight_threshold) &&
			CVAR_TO_FLOAT (rt_elight_threshold) >= 0;

		if (src->state & STRUCT_STATE_FOUND_WITH_MODEL)
		{
			accept = true;
		}

	    if ((src->state & STRUCT_STATE_FOUND_LIGHTSTYLE) && src->lightstyle > 0)
		{
			accept = true;
		}

		if (src->is_around_poi)
		{
			accept = true;
		}

		if (accept)
		{
			float intens = quake_intensity / CVAR_TO_FLOAT (rt_elight_normaliz);

			if (src->state & STRUCT_STATE_FOUND_LIGHTSTYLE)
			{
				float ls = (float)d_lightstylevalue[src->lightstyle] / 256.0f;
				intens *= CLAMP (0.0f, ls, 1.0f);
			}

			vec3_t color;
			RT_INIT_DEFAULT_LIGHT_COLOR (color);
			VectorScale (color, intens, color);
			RT_FIXUP_LIGHT_INTENSITY (color, true);

			RgSphericalLightUploadInfo info = {
				.uniqueID = (uint64_t)UINT16_MAX + i,
				.color = {color[0], color[1], color[2]},
				.position = {src->origin[0], src->origin[1], src->origin[2]},
				.radius = METRIC_TO_QUAKEUNIT (CVAR_TO_FLOAT (rt_elight_radius)),
			};

			// offset up a bit, so light is not inside the model itself
			if (src->state & STRUCT_STATE_FOUND_APPLY_OFFSET)
			{
				info.position.data[2] += METRIC_TO_QUAKEUNIT (0.75f);
			}

			RgResult r = rgUploadSphericalLight (vulkan_globals.instance, &info);
			RG_CHECK (r);

			RT_ClusterLightAdd (info.uniqueID, info.position.data);
		}
	}
}

// ============================================================================
// Q2RTX per-BSP-cluster light lists
//
// The world model's BSP leaves are used as clusters (vkQuake's PVS is
// leaf-indexed, exactly like the Q2RTX cluster visibility). Every frame the
// light uploads register (uniqueID, origin) here; RT_ClusterLightListsUpload
// then builds the per-cluster lists from the PVS and uploads them to the
// renderer, which resolves the unique IDs to its light-array indices.
// ============================================================================

#define RT_CLUSTER_MAX_LIGHTS    1024
#define RT_CLUSTER_MAX_PER_LIST  64    // must match Q2_LIGHT_LIST_MAX_PER_CELL
#define RT_CLUSTER_MAX_CLUSTERS  8192  // must match Q2_MAX_CLUSTERS

// A per-cluster light slot is stable across frames: it is assigned to a light
// unique ID the first time it is seen and kept while the light keeps appearing.
// The renderer's adaptive shadow statistics are keyed by (cluster, slot), so a
// stable slot keeps each light's hit/miss counters attributed to itself. Slots
// that stay unused for this many frames are recycled for new lights.
#define RT_CLUSTER_SLOT_RECYCLE_FRAMES 60
// Hole marker written into the slot-indexed per-cluster lists: it never matches
// a real light unique ID, resolves to an invalid light-array index on the
// renderer side, and the shader skips it (mass contribution zero).
#define RT_CLUSTER_INVALID_LIGHT       (~0ull)

typedef struct rt_cluster_light_s
{
	uint64_t uniqueID;
	vec3_t   origin;
} rt_cluster_light_t;

static rt_cluster_light_t rt_cluster_lights[RT_CLUSTER_MAX_LIGHTS];
static int rt_cluster_light_count;
static qboolean rt_cluster_dropped_warned;
static qboolean rt_cluster_perlist_warned;

// Per-cluster slot state. Shared between the upload (RT_ClusterLightListsUpload)
// and the rt_light_report diagnostics. See the upload for the stability rules.
static uint64_t *rt_cluster_slot_uids = NULL;
static uint32_t *rt_cluster_slot_stamp = NULL;
static uint8_t  *rt_cluster_slot_fill = NULL;
static int       rt_cluster_slot_alloc = 0;
static int       rt_cluster_last_clusters = 0;
static uint32_t  rt_cluster_frame_stamp = 0;
static uint32_t *rt_cluster_offsets = NULL;
static uint64_t *rt_cluster_list = NULL;
static int       rt_cluster_list_alloc = 0;
static vec3_t    rt_cluster_vieworg;

// Diagnostics for rt_light_report. Purely informational: none of this changes
// which lights end up in the per-cluster lists.
typedef struct rt_light_diag_s
{
	uint64_t uniqueID;
	vec3_t   origin;
	qboolean resolved;    // an open leaf was found for the light origin
	int      granted;     // clusters that handed the light a slot
	int      denied;      // clusters that were already full
} rt_light_diag_t;

static rt_light_diag_t rt_light_diag[RT_CLUSTER_MAX_LIGHTS];
static int rt_light_diag_count;
static int rt_light_diag_unresolved;
static int rt_light_diag_granted;
static int rt_light_diag_denied;

void RT_ClusterLightListsReset (void)
{
	rt_cluster_light_count = 0;
	rt_light_diag_count = 0;
	rt_light_diag_unresolved = 0;
	rt_light_diag_granted = 0;
	rt_light_diag_denied = 0;
	VectorCopy (r_refdef.vieworg, rt_cluster_vieworg);
}

void RT_ClusterLightAdd (uint64_t uniqueID, const vec3_t origin)
{
	if (rt_cluster_light_count >= RT_CLUSTER_MAX_LIGHTS)
	{
		if (!rt_cluster_dropped_warned)
		{
			Con_DWarning ("RT: light count exceeded RT_CLUSTER_MAX_LIGHTS (%i), "
				"some lights will not be sampled by the RT renderer.\n",
				RT_CLUSTER_MAX_LIGHTS);
			rt_cluster_dropped_warned = true;
		}
		return;
	}

	// deduplicate by unique ID (a light can be uploaded from several paths)
	for (int i = 0; i < rt_cluster_light_count; i++)
	{
		if (rt_cluster_lights[i].uniqueID == uniqueID)
			return;
	}

	rt_cluster_lights[rt_cluster_light_count].uniqueID = uniqueID;
	VectorCopy (origin, rt_cluster_lights[rt_cluster_light_count].origin);

	rt_light_diag[rt_cluster_light_count].uniqueID = uniqueID;
	VectorCopy (origin, rt_light_diag[rt_cluster_light_count].origin);
	rt_light_diag[rt_cluster_light_count].resolved = false;
	rt_light_diag[rt_cluster_light_count].granted = 0;
	rt_light_diag[rt_cluster_light_count].denied = 0;
	rt_light_diag_count = rt_cluster_light_count + 1;

	rt_cluster_light_count++;
}

/*
A light origin that lands in the solid leaf has no PVS of its own, and
Mod_LeafPVS() answers "visible from every cluster" for it (Mod_NoVisPVS).
Registering such a light would insert it into every cluster of the map and
exhaust the RT_CLUSTER_MAX_PER_LIST budget everywhere, permanently starving
every light that happens to be registered later. Emissive surface centers and
light-fixture centroids routinely land exactly on, or just inside, world
geometry, so probe a small neighbourhood for an open leaf before giving up.
Returns NULL when the light is genuinely buried in solid and illuminates
nothing.
*/
static mleaf_t *RT_ResolveLightLeaf (const vec3_t origin, qmodel_t *wm)
{
	static const vec3_t probeDirs[6] = {
		{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
	};
	static const float probeDists[3] = {2.0f, 8.0f, 24.0f};

	mleaf_t *leaf = Mod_PointInLeaf ((float *)origin, wm);
	if (leaf && leaf != wm->leafs && leaf->contents != CONTENTS_SOLID)
		return leaf;

	for (int d = 0; d < 3; d++)
	{
		for (int i = 0; i < 6; i++)
		{
			vec3_t probe;
			probe[0] = origin[0] + probeDists[d] * probeDirs[i][0];
			probe[1] = origin[1] + probeDists[d] * probeDirs[i][1];
			probe[2] = origin[2] + probeDists[d] * probeDirs[i][2];
			leaf = Mod_PointInLeaf (probe, wm);
			if (leaf && leaf != wm->leafs && leaf->contents != CONTENTS_SOLID)
				return leaf;
		}
	}

	return NULL;
}

/*
Assign a stable slot to uid in cluster c and stamp it as present this frame.
The slot is reused if the light already owns one, recycled if a stale one is
free, or freshly appended when the cluster still has room. If the cluster is
full the light simply is not sampled there this frame (warned once globally).
Returns true when the light is in the cluster's list this frame.
*/
static qboolean RT_ClusterAssignSlot (int c, uint64_t uid, uint64_t *slotUids,
	uint32_t *slotStamp, uint8_t *slotFill, uint32_t frameStamp)
{
	uint64_t *cuids = slotUids + c * RT_CLUSTER_MAX_PER_LIST;
	uint32_t *cstamp = slotStamp + c * RT_CLUSTER_MAX_PER_LIST;
	const int cfill = slotFill[c];

	// Already assigned slot for this unique ID?
	for (int s = 0; s < cfill; s++)
	{
		if (cuids[s] == uid)
		{
			cstamp[s] = frameStamp;
			return true;
		}
	}

	// Recycle a slot that has been unused for a while.
	for (int s = 0; s < cfill; s++)
	{
		if (frameStamp - cstamp[s] > RT_CLUSTER_SLOT_RECYCLE_FRAMES)
		{
			cuids[s] = uid;
			cstamp[s] = frameStamp;
			return true;
		}
	}

	// Otherwise append a fresh slot.
	if (slotFill[c] < RT_CLUSTER_MAX_PER_LIST)
	{
		const int s = slotFill[c]++;
		cuids[s] = uid;
		cstamp[s] = frameStamp;
		return true;
	}

	if (!rt_cluster_perlist_warned)
	{
		Con_DWarning ("RT: a cluster reached the %i distinct-light limit, "
			"new lights are not sampled by the RT renderer.\n",
			RT_CLUSTER_MAX_PER_LIST);
		rt_cluster_perlist_warned = true;
	}

	return false;
}

void RT_ClusterLightListsUpload (void)
{
	qmodel_t *wm = cl.worldmodel;
	if (!wm || wm->type != mod_brush || !wm->leafs || wm->numleafs < 2)
		return;

	const int numClusters = wm->numleafs;
	if (numClusters > RT_CLUSTER_MAX_CLUSTERS)
		return;

	// Per-cluster light slots are STABLE across frames: a slot (0..63) is
	// assigned to each light unique ID the first time it is seen in a cluster
	// and kept while the light keeps appearing. The renderer's adaptive shadow
	// statistics are keyed by (cluster, slot), and the per-cluster lists are
	// written slot-indexed (holes as RT_CLUSTER_INVALID_LIGHT), so a light's
	// hit/miss counters are addressed by the SAME slot every frame, regardless
	// of registration order or of other lights appearing/leaving. Without this,
	// a light whose slot shifted read/wrote the counters of a neighbouring
	// light, which made the CDF mass oscillate and generated emissive triangle
	// lights toggle on/off ("timer" flicker of tlight07/tlight11 on e1m1).
	// The slot arrays live at file scope so rt_light_report can inspect them.
	const int slotCount = numClusters * RT_CLUSTER_MAX_PER_LIST;
	if (numClusters != rt_cluster_last_clusters)
	{
		if (slotCount > rt_cluster_slot_alloc)
		{
			rt_cluster_slot_uids = (uint64_t *)Mem_Realloc (rt_cluster_slot_uids, sizeof (uint64_t) * slotCount);
			rt_cluster_slot_stamp = (uint32_t *)Mem_Realloc (rt_cluster_slot_stamp, sizeof (uint32_t) * slotCount);
			rt_cluster_slot_fill = (uint8_t *)Mem_Realloc (rt_cluster_slot_fill, sizeof (uint8_t) * numClusters);
			rt_cluster_slot_alloc = slotCount;
		}
		// Reset the per-cluster slot state for the new map. slotUids/slotStamp
		// are gated by slotFill, so only slotFill and slotStamp need clearing
		// (slotStamp is read for recycling, never for stale slots).
		memset (rt_cluster_slot_fill, 0, sizeof (uint8_t) * numClusters);
		memset (rt_cluster_slot_stamp, 0, sizeof (uint32_t) * slotCount);
		rt_cluster_last_clusters = numClusters;
	}

	rt_cluster_frame_stamp++;

	if (numClusters > rt_cluster_list_alloc)
	{
		rt_cluster_offsets = (uint32_t *)Mem_Realloc (rt_cluster_offsets, sizeof (uint32_t) * (numClusters + 1));
		rt_cluster_list = (uint64_t *)Mem_Realloc (rt_cluster_list, sizeof (uint64_t) * numClusters * RT_CLUSTER_MAX_PER_LIST);
		rt_cluster_list_alloc = numClusters;
	}

	// Pass 1: find or assign the stable slot of every registered light in every
	// cluster it illuminates (via the PVS) and stamp it as present this frame.
	for (int li = 0; li < rt_cluster_light_count; li++)
	{
		rt_light_diag_t *diag = &rt_light_diag[li];

		// Never register a light from the solid leaf: it has no PVS of its
		// own, and Mod_LeafPVS() answers "visible from every cluster" for it,
		// which would exhaust the RT_CLUSTER_MAX_PER_LIST budget everywhere
		// and starve the lights registered later.
		mleaf_t *leaf = RT_ResolveLightLeaf (rt_cluster_lights[li].origin, wm);
		if (!leaf)
		{
			rt_light_diag_unresolved++;
			continue;
		}
		diag->resolved = true;

		const uint64_t uid = rt_cluster_lights[li].uniqueID;

		// A leaf with no compressed VIS row (a map that carries no visdata at
		// all, or a leaf the compiler gave no row) makes Mod_LeafPVS() answer
		// "visible from every cluster". Flooding all clusters again exhausts
		// the per-cluster slot budget map-wide and starves lights registered
		// later, so cap the fallback by distance instead: the light reaches
		// only clusters whose leaf bounds lie within rt_light_reach of it.
		if (!leaf->compressed_vis)
		{
			const float reach = METRIC_TO_QUAKEUNIT (CVAR_TO_FLOAT (rt_light_reach));
			const float reachSq = reach * reach;
			for (int c = 1; c < numClusters; c++)
			{
				const mleaf_t *cleaf = &wm->leafs[c];
				if (cleaf->contents == CONTENTS_SOLID)
					continue;

				// squared distance from the light origin to the leaf AABB
				const float *o = rt_cluster_lights[li].origin;
				float         d2 = 0.0f;
				for (int a = 0; a < 3; a++)
				{
					float d = 0.0f;
					if (o[a] < cleaf->minmaxs[a])
						d = cleaf->minmaxs[a] - o[a];
					else if (o[a] > cleaf->minmaxs[3 + a])
						d = o[a] - cleaf->minmaxs[3 + a];
					d2 += d * d;
				}
				if (d2 > reachSq)
					continue;

				if (RT_ClusterAssignSlot (c, uid, rt_cluster_slot_uids, rt_cluster_slot_stamp, rt_cluster_slot_fill, rt_cluster_frame_stamp))
					diag->granted++;
				else
					diag->denied++;
			}
			continue;
		}

		const byte *vis = Mod_LeafPVS (leaf, wm);
		for (int j = 0; j < (numClusters + 7) / 8; j++)
		{
			if (!vis[j])
				continue;
			for (int k = 0; k < 8; k++)
			{
				if (!(vis[j] & (1u << k)))
					continue;
				// PVS bit (j*8+k) -> leaf index (j*8+k)+1 (1-based, leaf 0
				// is the all-seeing solid leaf).
				const int c = (j << 3) + k + 1;
				if (c >= numClusters)
					continue;

				if (RT_ClusterAssignSlot (c, uid, rt_cluster_slot_uids, rt_cluster_slot_stamp, rt_cluster_slot_fill, rt_cluster_frame_stamp))
					diag->granted++;
				else
					diag->denied++;
			}
		}
	}

	rt_light_diag_granted = 0;
	rt_light_diag_denied = 0;
	for (int li = 0; li < rt_light_diag_count; li++)
	{
		rt_light_diag_granted += rt_light_diag[li].granted;
		rt_light_diag_denied += rt_light_diag[li].denied;
	}

	// Prefix sums over the allocated slot counts -> offsets.
	uint32_t total = 0;
	for (int c = 0; c < numClusters; c++)
	{
		rt_cluster_offsets[c] = total;
		total += (uint32_t)rt_cluster_slot_fill[c];
	}
	rt_cluster_offsets[numClusters] = total;

	// Pass 2: write the slot-indexed lists. Lights present this frame keep
	// their slot; absent ones leave a hole (RT_CLUSTER_INVALID_LIGHT) in their
	// slot, so downstream slots never shift and the shader's stats stay keyed
	// to the same light as last frame.
	for (int c = 0; c < numClusters; c++)
	{
		const int cfill = rt_cluster_slot_fill[c];
		uint64_t *dst = rt_cluster_list + rt_cluster_offsets[c];
		uint64_t *cuids = rt_cluster_slot_uids + c * RT_CLUSTER_MAX_PER_LIST;
		uint32_t *cstamp = rt_cluster_slot_stamp + c * RT_CLUSTER_MAX_PER_LIST;
		for (int s = 0; s < cfill; s++)
		{
			dst[s] = (cstamp[s] == rt_cluster_frame_stamp) ? cuids[s] : RT_CLUSTER_INVALID_LIGHT;
		}
	}

	RgClusterLightListsUploadInfo info = {
		.numClusters = (uint32_t)numClusters,
		.pOffsets = rt_cluster_offsets,
		.pLightUniqueIds = rt_cluster_list,
		.totalLightCount = total,
	};
	RgResult r = rgUploadClusterLightLists (vulkan_globals.instance, &info);
	RG_CHECK (r);
}

/*
================
RT_FormatLightId

Human readable name for a light unique ID. World surface lights carry the
surface index, so the emissive source texture can be printed as well - that is
what makes a report line match a texture in the editor.
================
*/
static void RT_FormatLightId (char *out, size_t outSize, uint64_t uid)
{
	const int      type      = (int)(uid >> 60);
	const int      triangle  = (int)((uid >> 48) & 0xFFFull);
	const int      surfindex = (int)((uid >> 32) & 0xFFFFull);
	const unsigned ent       = (unsigned)(uid & 0xFFFFFFFFull);
	const char    *texname   = NULL;

	if (type == 1 && ent == ENT_UNIQUEID_WORLD && cl.worldmodel && cl.worldmodel->type == mod_brush &&
		surfindex < cl.worldmodel->numsurfaces)
	{
		msurface_t *surf = &cl.worldmodel->surfaces[surfindex];
		if (surf->texinfo && surf->texinfo->texture)
			texname = surf->texinfo->texture->name;
	}

	if (type == 1 && texname)
		q_snprintf (out, outSize, "world  surf %-4i %s", surfindex, texname);
	else if (type == 1)
		q_snprintf (out, outSize, "brush  ent %-5u surf %-4i tri %i", ent, surfindex, triangle);
	else if (type == 2)
		q_snprintf (out, outSize, "alias  ent %u", ent);
	else if (type == 3)
		q_snprintf (out, outSize, "sprite ent %u", ent);
	else
		q_snprintf (out, outSize, "type %i uid %016llx", type, (unsigned long long)uid);
}

/*
================
RT_ClusterLightReport_f

Prints why each registered RT light may or may not be sampled by the renderer.

A light that reaches the renderer at all was registered (the emissive pass
produced it), so it is in the emissive list. The remaining ways for it to stay
dark are: no open leaf was found for its origin, or every cluster that the PVS
accepted it into was already at RT_CLUSTER_MAX_PER_LIST. Additionally a light
can be perfectly valid and yet not light the current view, because the viewer's
own cluster does not list it - that is the interesting case for "the same
texture lights up in one place of the map but not in another".

Columns:
  cls   yes when the light is listed in the viewer's own cluster
  pvs   clusters that accepted it
  no!   clusters that were already full and rejected it
  dist  distance from the camera in Quake units
  then the light ID and a verdict.

Run it while looking at the problem in game: the lists are filled by the world
pass, so the numbers describe the last rendered frame.
================
*/
static float RT_LightDiagDist (const rt_light_diag_t *d)
{
	const float dx = d->origin[0] - rt_cluster_vieworg[0];
	const float dy = d->origin[1] - rt_cluster_vieworg[1];
	const float dz = d->origin[2] - rt_cluster_vieworg[2];
	return sqrtf (dx * dx + dy * dy + dz * dz);
}

void RT_ClusterLightReport_f (void)
{
	const int maxLines = (Cmd_Argc () > 1) ? atoi (Cmd_Argv (1)) : 64;

	if (rt_cluster_last_clusters <= 0 || !rt_cluster_slot_fill)
	{
		Con_Printf ("RT lights: no cluster light state yet - load a map and look at the world first.\n");
		return;
	}

	Con_Printf ("RT lights: %i registered, %i dropped (no open leaf), %i cluster slots granted, %i denied\n",
		rt_cluster_light_count, rt_light_diag_unresolved, rt_light_diag_granted, rt_light_diag_denied);

	int             viewCluster = -1;
	int             viewFill = 0;
	const uint64_t *viewUids = NULL;

	if (cl.worldmodel && cl.worldmodel->type == mod_brush)
	{
		mleaf_t *viewleaf = Mod_PointInLeaf (rt_cluster_vieworg, cl.worldmodel);
		if (viewleaf && viewleaf != cl.worldmodel->leafs)
		{
			viewCluster = (int)(viewleaf - cl.worldmodel->leafs);
			viewFill = rt_cluster_slot_fill[viewCluster];
			viewUids = rt_cluster_slot_uids + viewCluster * RT_CLUSTER_MAX_PER_LIST;
		}
	}

	if (viewCluster < 0)
		Con_Printf ("camera cluster: unavailable (camera is not in the world)\n");
	else
	{
		int live = 0;
		for (int s = 0; s < viewFill; s++)
		{
			if (viewUids[s] != RT_CLUSTER_INVALID_LIGHT)
				live++;
		}
		Con_Printf ("camera cluster %i: %i/%i slots used, %i accepted this frame%s\n", viewCluster, viewFill,
			RT_CLUSTER_MAX_PER_LIST, live,
			(viewFill >= RT_CLUSTER_MAX_PER_LIST) ? "  *** FULL: further lights are dropped here ***" : "");
	}

	Con_Printf ("%-3s %5s %5s %8s  %-34s %s\n", "cls", "pvs", "no!", "dist", "light", "verdict");

	int shown = 0;

	// rt_light_report_filter narrows the table to one texture, and the rows are
	// printed nearest-first, so a filtered report describes the light the
	// player is actually looking at instead of the first N lights in
	// registration order. Sorted with an insertion sort over an index array
	// (rt_light_diag itself must keep its registration order).
	const char *filter = rt_light_report_filter.string;
	static int  order[RT_CLUSTER_MAX_LIGHTS];
	int         order_num = 0;

	for (int li = 0; li < rt_light_diag_count; li++)
	{
		if (filter[0])
		{
			char probe[64];
			RT_FormatLightId (probe, sizeof (probe), rt_light_diag[li].uniqueID);
			if (!strstr (probe, filter))
				continue;
		}
		order[order_num++] = li;
	}

	for (int i = 1; i < order_num; i++)
	{
		const int key = order[i];
		const float keydist = RT_LightDiagDist (&rt_light_diag[key]);
		int j = i - 1;
		while (j >= 0 && RT_LightDiagDist (&rt_light_diag[order[j]]) > keydist)
		{
			order[j + 1] = order[j];
			j--;
		}
		order[j + 1] = key;
	}

	if (filter[0])
	{
		Con_Printf ("filter \"%s\": %i of %i registered lights match\n", filter, order_num, rt_light_diag_count);
		if (order_num == 0)
			Con_Printf ("  (this texture registered no light at all - see the emissive pass above)\n");
	}

	for (int oi = 0; oi < order_num; oi++)
	{
		const rt_light_diag_t *d = &rt_light_diag[order[oi]];

		qboolean inView = false;
		for (int s = 0; s < viewFill; s++)
		{
			if (viewUids[s] == d->uniqueID)
			{
				inView = true;
				break;
			}
		}

		const float dist = RT_LightDiagDist (d);

		const char *verdict;
		if (!d->resolved)
			verdict = "DROPPED: no open leaf for the light origin";
		else if (!d->granted && d->denied)
			verdict = "DROPPED: every cluster was full";
		else if (!d->granted)
			verdict = "DROPPED: no cluster accepted it";
		else if (!inView)
			verdict = "ok, but camera cluster does not sample it";
		else
			verdict = "ok";

		if (shown >= maxLines)
			break;
		shown++;

		char id[64];
		RT_FormatLightId (id, sizeof (id), d->uniqueID);
		Con_Printf ("%-3s %5i %5i %8.0f  %-34s %s\n", inView ? "yes" : "-", d->granted, d->denied, dist, id, verdict);
	}

	if (shown < order_num)
		Con_Printf ("... %i more matches (rt_light_report <line count>%s to print more)\n",
			order_num - shown, filter[0] ? " <texture>" : "");
}