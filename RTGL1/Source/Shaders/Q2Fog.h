// Copyright (c) 2020-2021 Sultim Tsyrendashiev
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// Ported from Q2RTX (GPL v2): path_tracer_rgen.h::find_fog_volumes,
// path_tracer_transparency.glsl::evaluate_fog / alpha_blend_premultiplied.
// Adapted to the RTGL1 global uniform (flat fog arrays).

#ifndef Q2_FOG_H_
#define Q2_FOG_H_

// Global multiplier for the color of fog volumes (Q2RTX "pt_fog_brightness").
#define Q2_FOG_BRIGHTNESS 0.01

float q2FogVMin(vec3 v) { return min(v.x, min(v.y, v.z)); }
float q2FogVMax(vec3 v) { return max(v.x, max(v.y, v.z)); }

uvec2 q2PackHalf4x16(vec4 v) { return uvec2(packHalf2x16(v.xy), packHalf2x16(v.zw)); }
vec4 q2UnpackHalf4x16(uvec2 v) { return vec4(unpackHalf2x16(v.x), unpackHalf2x16(v.y)); }

// Loops over the fog volumes and finds the two closest ones along the ray.
// They are stored in the order of min distance in fog1 (closer) and fog2 (further away).
void q2FindFogVolumes(vec3 origin, vec3 dir, float tMin, float tMax,
                      out uvec4 fog1, out uvec4 fog2)
{
    fog1 = uvec4(0);
    fog2 = uvec4(0);

    const vec3 inv_dir = vec3(1.0) / dir;
    for (int i = 0; i < MAX_FOG_VOLUMES; i++)
    {
        if (globalUniform.fogIsActive[i] == 0u)
        {
            continue;
        }

        const vec3 mins = globalUniform.fogMins[i].xyz;
        const vec3 maxs = globalUniform.fogMaxs[i].xyz;
        const vec3 color = globalUniform.fogColor[i].xyz;
        const vec4 density = globalUniform.fogDensity[i];

        const vec3 t1 = (mins - origin) * inv_dir;
        const vec3 t2 = (maxs - origin) * inv_dir;
        float t_in = q2FogVMax(min(t1, t2));
        float t_out = q2FogVMin(max(t1, t2));
        t_in = max(t_in, tMin);
        t_out = min(t_out, tMax);

        if (t_out > t_in)
        {
            const vec2 first_t_min_max = unpackHalf2x16(fog1.w);
            const vec2 second_t_min_max = unpackHalf2x16(fog2.w);

            const bool replaces_first = t_in < first_t_min_max.x || first_t_min_max.y == 0.0;
            const bool replaces_second = t_in < second_t_min_max.x || second_t_min_max.y == 0.0;

            if (replaces_first || replaces_second)
            {
                uvec4 packed;
                packed.xy = q2PackHalf4x16(vec4(color * Q2_FOG_BRIGHTNESS, 0.0));
                packed.z = packHalf2x16(vec2(t_in, t_out));

                // Convert the volumetric density function into a 1D function along the ray
                const float density_variable = dot(density.xyz, dir) * 0.5;
                const float density_constant = dot(density.xyz, origin) + density.w;
                // Scale the density stored here because typical values are very small
                // (in fp16 denormal range)
                packed.w = packHalf2x16(vec2(density_variable, density_constant) * 65536.0);

                if (replaces_first)
                {
                    // Push fog1 to fog2, replace fog1 with the new volume
                    fog2 = fog1;
                    fog1 = packed;
                }
                else // if (replaces_second) -- must be true
                {
                    // Replace fog2 with the new volume
                    fog2 = packed;
                }
            }
        }
    }
}

// Analytic transmittance of a linear density fog volume between t1 and t2.
vec4 q2EvaluateFog(uvec4 fog, float t1, float t2)
{
    const vec2 fog_bounds = unpackHalf2x16(fog.z);
    const vec2 fog_density = unpackHalf2x16(fog.w) / 65536.0;
    const vec3 fog_color = q2UnpackHalf4x16(fog.xy).rgb;

    t1 = max(t1, fog_bounds.x);
    t2 = min(t2, fog_bounds.y);

    if (t1 >= t2)
    {
        return vec4(0);
    }

    // Solution to the differential equation: dL = -(a*t + b) dt,
    // where L is luminance and alpha is (1 - L_out / L_in),
    // a and b are the density function parameters.
    const float alpha = 1.0 - exp((t1 * t1 - t2 * t2) * fog_density.x + (t1 - t2) * fog_density.y);

    return vec4(fog_color * alpha, alpha);
}

vec4 q2AlphaBlendPremultiplied(vec4 src, vec4 dst)
{
    return vec4(src.rgb + dst.rgb * (1.0 - src.a), src.a + dst.a * (1.0 - src.a));
}

// Accumulated premultiplied fog over [0, tMax] for the two closest volumes on
// the ray (fog1 is closer, fog2 further). Used per ray segment; multiple
// segments are combined with q2AlphaBlendPremultiplied (nearest in front).
vec4 q2SegmentFog(uvec4 fog1, uvec4 fog2, float tMax)
{
    vec4 acc = vec4(0);
    if (fog2.w != 0u)
    {
        acc = q2AlphaBlendPremultiplied(q2EvaluateFog(fog2, 0.0, tMax), acc);
    }
    if (fog1.w != 0u)
    {
        acc = q2AlphaBlendPremultiplied(q2EvaluateFog(fog1, 0.0, tMax), acc);
    }
    return acc;
}

// Blends the two closest fog volumes along the ray over [0, tMax] into the surface color.
vec3 q2ApplyFog(uvec4 fog1, uvec4 fog2, float tMax, vec3 color)
{
    // Blend the further volume first
    vec4 acc = vec4(0);
    if (fog2.w != 0u)
    {
        acc = q2AlphaBlendPremultiplied(q2EvaluateFog(fog2, 0.0, tMax), acc);
    }

    // ...then the closer volume
    if (fog1.w != 0u)
    {
        acc = q2AlphaBlendPremultiplied(q2EvaluateFog(fog1, 0.0, tMax), acc);
    }

    return acc.rgb + color * (1.0 - acc.a);
}

#endif // Q2_FOG_H_
