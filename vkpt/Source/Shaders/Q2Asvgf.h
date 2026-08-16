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
// Ported from Q2RTX (GPL v2, Copyright (C) 2018 Christoph Schied,
// Copyright (C) 2019, NVIDIA CORPORATION) - shader/asvgf.glsl, utils.glsl.

#ifndef Q2_ASVGF_H_
#define Q2_ASVGF_H_

// Q2RTX-style ASVGF denoiser, adapted to the vkpt framebuffer/uniform framework.
//
// The ASVGF works on three lighting channels:
//   - LF:  indirect diffuse, stored as luma spherical harmonics (4 coeffs) in
//          YCoCg color space. 1/3 resolution.
//   - HF:  direct diffuse irradiance, packed RGBE.
//   - SPEC: demodulated specular irradiance, packed RGBE.
// These are produced from the vkpt ReSTIR outputs by CmQ2Adapter.comp.

#define Q2_GRAD_DWN 3
#define Q2_STRATUM_OFFSET_SHIFT 3
#define Q2_STRATUM_OFFSET_MASK ((1 << Q2_STRATUM_OFFSET_SHIFT) - 1)

// Q2RTX storage scales: colors are multiplied by these when stored to keep
// precision in 16-bit / 9-bit formats, and divided back during compositing.
#define Q2_STORAGE_SCALE_LF 1024.0
#define Q2_STORAGE_SCALE_HF 32.0
#define Q2_STORAGE_SCALE_SPEC 32.0
#define Q2_STORAGE_SCALE_HDR 128.0

// ASVGF tuning parameters. Base values are the Q2RTX cvars defaults, but the
// HF/SPEC antilag is reduced because the lighting input here is vkpt ReSTIR,
// which is noisier per-frame than the Q2RTX path tracer: the gradient compares
// two independent noisy samples (Q2RTX re-traces with the previous RNG seed,
// which cancels the noise), so in dark areas the gradient is noise-dominated
// and would otherwise collapse the temporal history. The spatial filters match
// the Q2RTX defaults (flt_atrous_depth 0.5, flt_atrous_lum_hf 16,
// flt_atrous_normal_spec 1) to maximize dark-area noise removal.
#define Q2_FLT_ANTILAG_HF 0.25
#define Q2_FLT_ANTILAG_LF 0.15
#define Q2_FLT_ANTILAG_SPEC 0.5
#define Q2_FLT_ANTILAG_SPEC_MOTION 0.004
#define Q2_FLT_GRAD_WEAPON 0.25
#define Q2_FLT_MIN_ALPHA_COLOR_HF 0.02
#define Q2_FLT_MIN_ALPHA_COLOR_LF 0.01
#define Q2_FLT_MIN_ALPHA_COLOR_SPEC 0.01
#define Q2_FLT_MIN_ALPHA_MOMENTS_HF 0.01
#define Q2_FLT_TEMPORAL_HF 1.0
#define Q2_FLT_TEMPORAL_LF 1.0
#define Q2_FLT_TEMPORAL_SPEC 1.0

// a-trous filter iterations for LF/HF/SPEC
#define Q2_ATROUS_ITERATIONS_LF 4
#define Q2_ATROUS_ITERATIONS_HF 4
#define Q2_ATROUS_ITERATIONS_SPEC 4
#define Q2_FLT_ATROUS_DEPTH 0.5
#define Q2_FLT_ATROUS_NORMAL_LF 8.0
#define Q2_FLT_ATROUS_NORMAL_HF 16.0
#define Q2_FLT_ATROUS_NORMAL_SPEC 1.0
#define Q2_FLT_ATROUS_LUM_HF 16.0
#define Q2_FLT_ATROUS_DEFLICKER_LF 0.75

const float Q2_GAUSSIAN_KERNEL[2][2] = {
    { 1.0 / 4.0, 1.0 / 8.0  },
    { 1.0 / 8.0, 1.0 / 16.0 }
};

const float Q2_WAVELET_FACTOR = 0.5;
const float Q2_WAVELET_KERNEL[2][2] = {
    { 1.0, Q2_WAVELET_FACTOR  },
    { Q2_WAVELET_FACTOR, Q2_WAVELET_FACTOR * Q2_WAVELET_FACTOR }
};

// Use macros for storage image access: GLSL requires image formats to match,
// so different-format images cannot be passed to the same function parameter.
// (Same workaround as Q2RTX's STORE_SH.)
#define Q2_LOAD_SH(img_shY, img_CoCg, p) (Q2SH(texelFetch(img_shY, p, 0), texelFetch(img_CoCg, p, 0).xy))
#define Q2_STORE_SH(img_shY, img_CoCg, p, sh) { imageStore(img_shY, p, (sh).shY); imageStore(img_CoCg, p, vec4((sh).CoCg, 0.0, 0.0)); }

// Q2RTX-style SH: luma in 4 SH coefficients + chroma (YCoCg).
// Named Q2SH to avoid a clash with the vkpt SH struct from SphericalHarmonics.h.
struct Q2SH
{
    vec4 shY;
    vec2 CoCg;
};

Q2SH q2InitSH()
{
    Q2SH r;
    r.shY = vec4(0.0);
    r.CoCg = vec2(0.0);
    return r;
}

void q2AccumulateSH(inout Q2SH accum, Q2SH b, float scale)
{
    accum.shY += b.shY * scale;
    accum.CoCg += b.CoCg * scale;
}

Q2SH q2MixSH(Q2SH a, Q2SH b, float s)
{
    Q2SH r;
    r.shY = mix(a.shY, b.shY, vec4(s));
    r.CoCg = mix(a.CoCg, b.CoCg, vec2(s));
    return r;
}

// Convert an RGB irradiance SH (per-channel L00,L1-1,L10,L11) into the
// Q2RTX YCoCg luma-SH + chroma representation used by the LF channel.
// shY = luma coefficients, CoCg = chroma from the DC (L00) terms.
//
// NOTE on coefficient order: vkpt stores SH as (L00, L1-1, L10, L11) in
// x,y,z,w per color channel, while the Q2RTX luma-SH is stored as
// vec4(L11, L1-1, L10, L00). The order must be swapped here.
Q2SH q2IrradianceToSH(const vec4 shR, const vec4 shG, const vec4 shB)
{
    Q2SH r;

    // Y = 0.25*R + 0.5*G + 0.25*B  (from the YCoCg transform, applied per coefficient)
    r.shY = vec4(
        0.25 * shR.w + 0.5 * shG.w + 0.25 * shB.w,  // L11
        0.25 * shR.y + 0.5 * shG.y + 0.25 * shB.y,  // L1-1
        0.25 * shR.z + 0.5 * shG.z + 0.25 * shB.z,  // L10
        0.25 * shR.x + 0.5 * shG.x + 0.25 * shB.x); // L00

    // chroma from the DC terms
    const float Co = shR.x - shB.x;
    const float Cg = shG.x - 0.5 * (shR.x + shB.x);
    r.CoCg = vec2(Co, Cg);

    return r;
}

// Evaluate the Q2RTX LF SH at a given normal, back to RGB irradiance.
vec3 q2SHToIrradiance(const Q2SH sh, const vec3 N)
{
    const float d = dot(sh.shY.xyz, N);
    float Y = 2.0 * (1.023326 * d + 0.886226 * sh.shY.w);
    Y = max(Y, 0.0);

    // Guard against chroma explosion when the DC (L00) term is near zero
    const float chromaScale = clamp(Y * 0.282095 / (sh.shY.w + 1e-6), 0.0, 8.0);
    const vec2 CoCg = sh.CoCg * chromaScale;

    const float T = Y - CoCg.y * 0.5;
    const float G = CoCg.y + T;
    const float B = T - CoCg.x * 0.5;
    const float R = B + CoCg.x;

    return max(vec3(R, G, B), vec3(0.0));
}

// Q2RTX packed RGBE (9 bit mantissa, 5 bit exponent)
uint q2PackRGBE(vec3 v)
{
    vec3 va = max(vec3(0.0), v);
    float max_abs = max(va.r, max(va.g, va.b));
    if (max_abs == 0.0)
    {
        return 0u;
    }

    float exponent = floor(log2(max_abs));

    uint result = uint(clamp(exponent + 20.0, 0.0, 31.0)) << 27;

    float scale = pow(2.0, -exponent) * 256.0;
    uvec3 vu = min(uvec3(511), uvec3(round(va * scale)));
    result |= vu.r;
    result |= vu.g << 9;
    result |= vu.b << 18;

    return result;
}

vec3 q2UnpackRGBE(uint x)
{
    int exponent = int(x >> 27) - 20;
    float scale = pow(2.0, float(exponent)) / 256.0;

    vec3 v;
    v.r = float(x & 0x1ffu) * scale;
    v.g = float((x >> 9) & 0x1ffu) * scale;
    v.b = float((x >> 18) & 0x1ffu) * scale;

    return v;
}

// Relative lighting difference between current and previous frame (Q2RTX)
float q2GetGradient(float l_curr, float l_prev)
{
    float l_max = max(l_curr, l_prev);

    if (l_max == 0.0)
    {
        return 0.0;
    }

    float ret = abs(l_curr - l_prev) / l_max;
    ret *= ret; // make small changes less significant

    return ret;
}

// Convert a checkerboarded pixel position (left/right fields) to flat-screen position
ivec2 q2CheckerToFlat(ivec2 pos, int width)
{
    const int half_width = width / 2;
    const bool is_even_checkerboard = pos.x < half_width;

    return ivec2(
        is_even_checkerboard
            ? (pos.x * 2) + (pos.y & 1)
            : ((pos.x - half_width) * 2) + ((pos.y & 1) ^ 1),
        pos.y);
}

// Convert a flat-screen (regular) pixel position to checkerboarded (left/right fields)
ivec2 q2FlatToChecker(ivec2 pos, int width)
{
    const int half_width = width / 2;
    const bool is_even_checkerboard = (pos.x & 1) == (pos.y & 1);

    return ivec2(
        (pos.x / 2) + (is_even_checkerboard ? 0 : half_width),
        pos.y);
}

#endif // Q2_ASVGF_H_
