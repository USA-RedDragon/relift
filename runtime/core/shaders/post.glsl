/**
 * @file post.glsl
 * @brief What every pass of the core picture pipeline (post.c) shares: the
 * buffers, the push constants and texture sampling. Pictures are buffers of
 * one RGBA8 word per pixel (red in the low byte), rows packed, row 0 at the
 * top. Sampling is done here in the shader rather than by a sampler, with
 * clamped edges and pixel centers at .5, so every device computes the same
 * filter weights in full float precision.
 */

layout(local_size_x = 8, local_size_y = 8) in;

layout(std430, set = 0, binding = 0) readonly buffer ColorIn { uint color_in[]; };
layout(std430, set = 0, binding = 1) writeonly buffer ColorOut { uint color_out[]; };
layout(std430, set = 0, binding = 2) buffer Edges { uint edges[]; };
layout(std430, set = 0, binding = 3) buffer Blend { uint blend[]; };
layout(std430, set = 0, binding = 4) readonly buffer Area { uint area[]; };
layout(std430, set = 0, binding = 5) readonly buffer Search { uint search[]; };

/** @brief The picture size and the pass's tuning (post.c fills it). */
layout(push_constant) uniform Params {
    ivec2 size;
    float threshold;
    int max_search_steps;
    int max_search_steps_diag;
    int corner_rounding;
} pc;

const int TEX_COLOR = 0;
const int TEX_EDGES = 2;
const int TEX_BLEND = 3;
const int TEX_AREA = 4;
const int TEX_SEARCH = 5;

const ivec2 AREA_SIZE = ivec2(160, 560);
const ivec2 SEARCH_SIZE = ivec2(64, 16);

/** @brief The size of a texture in texels. */
ivec2 tex_size(int tex) { return tex == TEX_AREA ? AREA_SIZE : tex == TEX_SEARCH ? SEARCH_SIZE : pc.size; }

/** @brief One texel of a texture, its coordinates clamped to the edges. */
vec4 tex_fetch(int tex, ivec2 p)
{
    ivec2 s = tex_size(tex);
    p = clamp(p, ivec2(0), s - 1);
    int i = p.y * s.x + p.x;
    uint w;
    switch (tex) {
    case TEX_COLOR: w = color_in[i]; break;
    case TEX_EDGES: w = edges[i]; break;
    case TEX_BLEND: w = blend[i]; break;
    case TEX_AREA: w = area[i]; break;
    default: w = search[i]; break;
    }
    return unpackUnorm4x8(w);
}

/** @brief A bilinear sample at uv (0..1 across the texture). */
vec4 tex_linear(int tex, vec2 uv)
{
    vec2 p = uv * vec2(tex_size(tex)) - 0.5;
    vec2 f0 = floor(p);
    vec2 f = p - f0;
    ivec2 i = ivec2(f0);
    vec4 a = tex_fetch(tex, i), b = tex_fetch(tex, i + ivec2(1, 0));
    vec4 c = tex_fetch(tex, i + ivec2(0, 1)), d = tex_fetch(tex, i + ivec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

/** @brief A nearest sample at uv. */
vec4 tex_point(int tex, vec2 uv) { return tex_fetch(tex, ivec2(floor(uv * vec2(tex_size(tex))))); }

/** @brief The pixel this invocation writes, or false past the picture's edge. */
bool this_pixel(out ivec2 p)
{
    p = ivec2(gl_GlobalInvocationID.xy);
    return p.x < pc.size.x && p.y < pc.size.y;
}
