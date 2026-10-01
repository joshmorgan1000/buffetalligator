#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
import bridge_native;
struct AlligatorPush {
    uint64_t vulkan_table_address;
    uint64_t vulkan_pool_address;
};
layout(local_size_x=16,local_size_y=4,local_size_z=1) in;
void main(uniform AlligatorPush parameters) {
    uint64_t region = bridge_region(parameters.vulkan_pool_address, 24u);
    ((uint64_t*)parameters.vulkan_table_address)[gl_LocalInvocationIndex] = region;
}
