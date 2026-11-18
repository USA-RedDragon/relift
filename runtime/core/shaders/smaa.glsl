/**
 * @file smaa.glsl
 * @brief SMAA 1x (iryoku/smaa, MIT; runtime/third_party/smaa) on the core
 * pipeline's buffers: SMAA.hlsl itself, included through its SMAA_CUSTOM_SL
 * porting macros. A texture is the number of its buffer (post.glsl) and
 * every sample goes through tex_linear or tex_point. The quality preset's
 * numbers come from the push constants; SMAA_DISABLE_DIAG_DETECTION and
 * SMAA_DISABLE_CORNER_DETECTION are set at compile time by the variant (the
 * low and medium presets). A pass's discard (no edge here) returns zero
 * edges, the same thing written to the edges buffer.
 */
#include "post.glsl"

#define SMAA_CUSTOM_SL 1
#define SMAA_RT_METRICS vec4(1.0 / vec2(pc.size), vec2(pc.size))
#define SMAA_THRESHOLD pc.threshold
#define SMAA_MAX_SEARCH_STEPS pc.max_search_steps
#define SMAA_MAX_SEARCH_STEPS_DIAG pc.max_search_steps_diag
#define SMAA_CORNER_ROUNDING pc.corner_rounding

#define SMAATexture2D(tex) int tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex_linear(tex, coord)
#define SMAASampleLevelZeroPoint(tex, coord) tex_point(tex, coord)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex_linear(tex, (coord) + vec2(offset) * SMAA_RT_METRICS.xy)
#define SMAASample(tex, coord) tex_linear(tex, coord)
#define SMAASamplePoint(tex, coord) tex_point(tex, coord)
#define SMAASampleOffset(tex, coord, offset) tex_linear(tex, (coord) + vec2(offset) * SMAA_RT_METRICS.xy)
#define SMAA_FLATTEN
#define SMAA_BRANCH
#define lerp(a, b, t) mix(a, b, t)
#define saturate(a) clamp(a, 0.0, 1.0)
#define mad(a, b, c) ((a) * (b) + (c))
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define bool2 bvec2
#define bool3 bvec3
#define bool4 bvec4
#define discard return vec2(0.0)

#include "SMAA.hlsl"

/** @brief The texture coordinate of the center of pixel p. */
vec2 center_uv(ivec2 p) { return (vec2(p) + 0.5) / vec2(pc.size); }
