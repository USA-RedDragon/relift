#version 460
layout(location = 0) in vec4 o;
layout(location = 1) in vec2 o2;
layout(location = 0) out vec4 c;
void main() { c = o + vec4(o2, 0, 0) * 1000.0; }
