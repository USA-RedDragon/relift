#version 450
// Draws a captured vertex: its first 16 bytes are the position.
layout(location = 0) in vec4 p;
layout(location = 0) out vec4 c;
void main() { gl_Position = vec4(p.xy, 0.5, 1.0); gl_PointSize = 1.0; c = p; }
