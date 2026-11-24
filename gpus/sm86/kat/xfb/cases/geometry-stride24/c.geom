#version 450
// Points in, at most one point out, capturing generic 0 x, y, z, generic 1 x and generic 2 x, y (stride 24), the
// layout of a game's second capturing geometry program.
layout(points) in;
layout(points, max_vertices = 1) out;
layout(location = 0) in vec4 v[];
layout(location = 0, xfb_buffer = 0, xfb_stride = 24, xfb_offset = 0) out vec3 a;
layout(location = 1, xfb_buffer = 0, xfb_offset = 12) out float b;
layout(location = 2, xfb_buffer = 0, xfb_offset = 16) out vec2 c;
void main() {
    a = v[0].xyz * 2.0;
    b = v[0].w + 1.0;
    c = v[0].yx;
    gl_Position = v[0];
    EmitVertex();
}
