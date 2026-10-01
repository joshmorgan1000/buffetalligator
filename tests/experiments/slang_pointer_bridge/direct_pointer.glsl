#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
layout(local_size_x=16,local_size_y=4,local_size_z=1) in;
layout(push_constant) uniform Push { uint64_t table; uint64_t directory; } push;
void main() {
    uint* pointer = (uint*)push.table;
    pointer[gl_GlobalInvocationID.x] = 123u;
}
