// Shared constants for the Q2RTX-style noise-aware tone mapper.
// Ported from Quake 2 RTX (tone_mapping_utils.glsl), GPL v2.
// Must be included AFTER ShaderCommonGLSLFunc.h (needs COMPUTE_LUM_HISTOGRAM_BIN_COUNT).

#ifndef TONEMAPPING_UTILS_GLSL_
#define TONEMAPPING_UTILS_GLSL_

#define HISTOGRAM_BINS COMPUTE_LUM_HISTOGRAM_BIN_COUNT

// Conversion from floating-point to fixed-point for atomic histogram adds.
#define FIXED_POINT_FRAC_BITS 7
#define FIXED_POINT_FRAC_MULTIPLIER (1 << FIXED_POINT_FRAC_BITS)

// Maximum log (base 2) luminance values (photographic stops) represented in
// the tone mapping histogram. Anything below min_log_luminance is not
// counted and becomes inky black on screen.
const float min_log_luminance = -24.0;
const float max_log_luminance = 8.0;

// Map [min_log_luminance, max_log_luminance] to [0, 1] with a single FMA:
//   x/(max-min) - min/(max-min)
const float log_luminance_scale = 1.0 / (max_log_luminance - min_log_luminance);
const float log_luminance_bias = -min_log_luminance * log_luminance_scale;

#endif // TONEMAPPING_UTILS_GLSL_
