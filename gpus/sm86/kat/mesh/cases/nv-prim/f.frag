#version 460
#extension GL_NV_mesh_shader : require
layout(location = 0) in vec4 o;
layout(location = 1) perprimitiveNV in vec4 q;
layout(location = 0) out vec4 c;
void main() { c = o + q * 100.0 + vec4(float(gl_PrimitiveID) * 1000.0); }
