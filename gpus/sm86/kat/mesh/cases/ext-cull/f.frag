#version 460
#extension GL_EXT_mesh_shader : require
layout(location = 0) in vec4 o;
layout(location = 2) perprimitiveEXT flat in uint r;
layout(location = 0) out vec4 c;
void main() { c = o + vec4(float(r), float(gl_PrimitiveID), 0, 0) * 1000.0; }
