#version 450
layout(location = 0) out vec4 v;
void main() { float i = float(gl_VertexIndex); v = vec4(i * 0.0625 - 0.5, 0.25 - i * 0.125, i, i * 0.5); gl_Position = v; }
