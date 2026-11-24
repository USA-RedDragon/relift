#version 450
// Triangles adjacency in, one point out per primitive, capturing generic 0 x, y, z and generic 1 x (stride 16): each input vertex
// weighted by its place (the weights summing to 1), so the capture shows which vertices the primitive got and in what order.
layout(triangles_adjacency) in;
layout(points, max_vertices = 1) out;
layout(location = 0) in vec4 v[];
layout(location = 0, xfb_buffer = 0, xfb_stride = 16, xfb_offset = 0) out vec3 a;
layout(location = 1, xfb_buffer = 0, xfb_offset = 12) out float b;
void main() {
    vec4 s = (v[0] * 1.0 + v[1] * 2.0 + v[2] * 3.0 + v[3] * 4.0 + v[4] * 5.0 + v[5] * 6.0) / 21.0;
    a = s.xyz;
    b = s.w + float(gl_PrimitiveIDIn);
    gl_Position = vec4(v[0].xy, 0.0, 1.0);
    EmitVertex();
}
