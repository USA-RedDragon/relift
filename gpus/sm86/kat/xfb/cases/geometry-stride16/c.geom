#version 450
// Points in, at most one point out, capturing generic 0 x, y, z and generic 1 x (stride 16), the layout of a
// game's capturing geometry program.
layout(points) in;
layout(points, max_vertices = 1) out;
layout(location = 0) in vec4 v[];
layout(location = 0, xfb_buffer = 0, xfb_stride = 16, xfb_offset = 0) out vec3 a;
layout(location = 1, xfb_buffer = 0, xfb_offset = 12) out float b;
layout(location = 2) out vec2 uncaptured;
void main() {
    a = v[0].xyz * 0.5 + vec3(0.25);
    b = v[0].w - v[0].x;
    uncaptured = v[0].zw;
    gl_Position = v[0];
    EmitVertex();
}
