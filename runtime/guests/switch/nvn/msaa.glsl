#version 450
// Moves a multisample texture's samples between the host's multisample image
// and the guest's storage (nvn_msaa.c). The guest stores a
// multisample texture as a single-sample one gw by gh times its size (the
// "twin" here): sample s of pixel (x, y) is the texel at
// (x * gw + cx, y * gh + cy), where cells holds cx | cy << 2 in bits 4s to
// 4s + 3 (the encoding of Mesa's NVK, src/nouveau/nil/su_info.rs
// compute_ms_table).
//
// EXPAND (fragment, run per sample): writes each sample of the multisample
// image, bound as a render target of the unsigned integer format of its
// texel size, from the twin's texel, so every bit moves unchanged.
// GATHER (compute): reads each sample of the multisample image through its
// unsigned integer view and stores it to the twin's texel.
// RESOLVE (fragment): Downsample's mean of the n samples whose indices are
// in bits 4i to 4i + 3 of cells, read through the image's own format.
#extension GL_EXT_samplerless_texture_functions : require

layout(push_constant) uniform Params {
    uint cells;
    uint gw, gh;
    int layer;
    uint width, height;
    uint n;
} p;

#if defined(EXPAND)
layout(binding = 0) uniform utexture2DArray twin;
layout(location = 0) out uvec4 o;
void main()
{
    uint c = (p.cells >> (4u * uint(gl_SampleID))) & 15u;
    ivec2 q = ivec2(gl_FragCoord.xy) * ivec2(p.gw, p.gh) + ivec2(c & 3u, c >> 2);
    o = texelFetch(twin, ivec3(q, p.layer), 0);
}
#elif defined(GATHER)
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(binding = 0) uniform utexture2DMSArray ms;
layout(binding = 1) writeonly uniform uimage2DArray twin;
void main()
{
    uvec3 g = gl_GlobalInvocationID;
    if (g.x >= p.width || g.y >= p.height)
        return;
    for (uint s = 0u; s < p.gw * p.gh; s++) {
        uint c = (p.cells >> (4u * s)) & 15u;
        ivec2 q = ivec2(g.xy) * ivec2(p.gw, p.gh) + ivec2(c & 3u, c >> 2);
        imageStore(twin, ivec3(q, int(g.z)), texelFetch(ms, ivec3(g.xy, int(g.z)), int(s)));
    }
}
#elif defined(RESOLVE)
layout(binding = 0) uniform texture2DMSArray ms;
layout(location = 0) out vec4 o;
void main()
{
    ivec3 q = ivec3(ivec2(gl_FragCoord.xy), p.layer);
    vec4 a = vec4(0.0);
    for (uint i = 0u; i < p.n; i++)
        a += texelFetch(ms, q, int((p.cells >> (4u * i)) & 15u));
    o = a * (1.0 / float(p.n));
}
#else
// The full-screen triangle the fragment passes draw.
void main()
{
    vec2 v = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(v * 2.0 - 1.0, 0.0, 1.0);
}
#endif
