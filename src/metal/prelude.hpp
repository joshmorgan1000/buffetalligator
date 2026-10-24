#pragma once
/** --------------------------------------------------------------------------------------------------------- Metal Shader Prelude
 * @file prelude.hpp
 * @brief Defines native MSL Chomp methods within each invocation's implicit address context.
 */
#include <string_view>

namespace buffetalligator {
inline constexpr std::string_view METAL_SHADER_CORE = R"metal(
#include <metal_stdlib>
using namespace metal;
struct KernelPush { ulong vulkan_table_address; ulong vulkan_pool_address; };
struct Chomp { uint id; };
struct GPUBufRef { ulong address; uint size; uint offset; };
struct AlligatorProgram {
    KernelPush vulkan_push;
    uint3 gl_WorkGroupID;
    uint3 gl_NumWorkGroups;
    uint3 gl_LocalInvocationID;
    uint gl_LocalInvocationIndex;
    uint3 gl_GlobalInvocationID;
    Chomp gpu_chomp(uint index) { return Chomp{index}; }
    ulong chomp_table(Chomp chomp) {
        return reinterpret_cast<device const ulong*>(vulkan_push.vulkan_pool_address)[((chomp.id >> 6u) & 1023u) * 2u];
    }
    GPUBufRef chomp_ref(Chomp chomp) {
        return reinterpret_cast<device const GPUBufRef*>(chomp_table(chomp))[chomp.id >> 16u];
    }
    ulong chomp_address(Chomp chomp) {
        GPUBufRef record = chomp_ref(chomp);
        return record.address + (ulong(record.offset) << 6u);
    }
    bool chomp_is_null(Chomp chomp) { return chomp.id == 0xFFFFFFFFu; }
    ulong chomp_size(Chomp chomp) { return ulong(chomp_ref(chomp).size) << 6u; }
    Chomp chomp_read(ulong address) { return gpu_chomp(*reinterpret_cast<device const uint*>(address)); }
    ulong gpu_chomp_address(uint index) { return chomp_address(gpu_chomp(index)); }
    ulong gpu_chomp_size(uint index) { return chomp_size(gpu_chomp(index)); }
    uint chomp_load_u32(Chomp chomp, uint index) { return reinterpret_cast<device const uint*>(chomp_address(chomp))[index]; }
    int chomp_load_i32(Chomp chomp, uint index) { return reinterpret_cast<device const int*>(chomp_address(chomp))[index]; }
    float chomp_load_f32(Chomp chomp, uint index) { return reinterpret_cast<device const float*>(chomp_address(chomp))[index]; }
    ulong chomp_load_u64(Chomp chomp, uint index) { return reinterpret_cast<device const ulong*>(chomp_address(chomp))[index]; }
    long chomp_load_i64(Chomp chomp, uint index) { return reinterpret_cast<device const long*>(chomp_address(chomp))[index]; }
    float4 chomp_load_f32x4(Chomp chomp, uint index) { return reinterpret_cast<device const float4*>(chomp_address(chomp))[index]; }
    uint4 chomp_load_u32x4(Chomp chomp, uint index) { return reinterpret_cast<device const uint4*>(chomp_address(chomp))[index]; }
    int4 chomp_load_i32x4(Chomp chomp, uint index) { return reinterpret_cast<device const int4*>(chomp_address(chomp))[index]; }
    void chomp_store_u32(Chomp chomp, uint index, uint value) { reinterpret_cast<device uint*>(chomp_address(chomp))[index] = value; }
    void chomp_store_i32(Chomp chomp, uint index, int value) { reinterpret_cast<device int*>(chomp_address(chomp))[index] = value; }
    void chomp_store_f32(Chomp chomp, uint index, float value) { reinterpret_cast<device float*>(chomp_address(chomp))[index] = value; }
    void chomp_store_u64(Chomp chomp, uint index, ulong value) { reinterpret_cast<device ulong*>(chomp_address(chomp))[index] = value; }
    void chomp_store_i64(Chomp chomp, uint index, long value) { reinterpret_cast<device long*>(chomp_address(chomp))[index] = value; }
    void chomp_store_f32x4(Chomp chomp, uint index, float4 value) { reinterpret_cast<device float4*>(chomp_address(chomp))[index] = value; }
    void chomp_store_u32x4(Chomp chomp, uint index, uint4 value) { reinterpret_cast<device uint4*>(chomp_address(chomp))[index] = value; }
    void chomp_store_i32x4(Chomp chomp, uint index, int4 value) { reinterpret_cast<device int4*>(chomp_address(chomp))[index] = value; }
    uint chomp_load_u16(Chomp chomp, uint index) { return (chomp_load_u32(chomp, index >> 1u) >> ((index & 1u) * 16u)) & 0xFFFFu; }
    int chomp_load_i16(Chomp chomp, uint index) { return int(short(chomp_load_u16(chomp, index))); }
    uint chomp_load_u8(Chomp chomp, uint index) { return (chomp_load_u32(chomp, index >> 2u) >> ((index & 3u) * 8u)) & 0xFFu; }
    int chomp_load_i8(Chomp chomp, uint index) { return int(char(chomp_load_u8(chomp, index))); }
    float chomp_load_f16(Chomp chomp, uint index) { return float(as_type<half>(ushort(chomp_load_u16(chomp, index)))); }
    float chomp_load_bf16(Chomp chomp, uint index) { return as_type<float>(chomp_load_u16(chomp, index) << 16u); }
    float chomp_load_e5m2(Chomp chomp, uint index) { return float(as_type<half>(ushort(chomp_load_u8(chomp, index) << 8u))); }
    void chomp_store_u16(Chomp chomp, uint index, uint value) {
        device atomic_uint* word = reinterpret_cast<device atomic_uint*>(chomp_address(chomp) + ulong((index * 2u) & ~3u));
        uint shift = (index & 1u) * 16u;
        atomic_fetch_and_explicit(word, ~(0xFFFFu << shift), memory_order_relaxed);
        atomic_fetch_or_explicit(word, (value & 0xFFFFu) << shift, memory_order_relaxed);
    }
    void chomp_store_u8(Chomp chomp, uint index, uint value) {
        device atomic_uint* word = reinterpret_cast<device atomic_uint*>(chomp_address(chomp) + ulong(index & ~3u));
        uint shift = (index & 3u) * 8u;
        atomic_fetch_and_explicit(word, ~(0xFFu << shift), memory_order_relaxed);
        atomic_fetch_or_explicit(word, (value & 0xFFu) << shift, memory_order_relaxed);
    }
    void chomp_store_i16(Chomp chomp, uint index, int value) { chomp_store_u16(chomp, index, uint(value)); }
    void chomp_store_i8(Chomp chomp, uint index, int value) { chomp_store_u8(chomp, index, uint(value)); }
    void chomp_store_f16(Chomp chomp, uint index, float value) { chomp_store_u16(chomp, index, as_type<ushort>(half(value))); }
    void chomp_store_bf16(Chomp chomp, uint index, float value) { chomp_store_u16(chomp, index, as_type<uint>(value) >> 16u); }
    void chomp_store_e5m2(Chomp chomp, uint index, float value) { chomp_store_u8(chomp, index, as_type<ushort>(half(value)) >> 8u); }
    uint fp8_to_f16_bits(uint code, uint mantissa_bits) { return ((code & 0x80u) << 8u) | ((code & 0x7Fu) << (10u - mantissa_bits)); }
    uint f16_bits_to_fp8(uint bits, uint mantissa_bits) { return ((bits >> 8u) & 0x80u) | ((bits >> (10u - mantissa_bits)) & 0x7Fu); }
    uint4 fp8x4_codes(uint word) { return uint4(word & 0xFFu, (word >> 8u) & 0xFFu, (word >> 16u) & 0xFFu, word >> 24u); }
    float4 fp8x4_to_f32x4(uint word, uint mantissa_bits, float scale) {
        uint4 codes = fp8x4_codes(word);
        ushort4 bits = ushort4(((codes & 0x80u) << 8u) | ((codes & 0x7Fu) << (10u - mantissa_bits)));
        return float4(as_type<half4>(bits)) * scale;
    }
    uint fp8x4_from_f32x4(float4 values, uint mantissa_bits, float inverse_scale) {
        uint4 bits = uint4(as_type<ushort4>(half4(values * inverse_scale)));
        uint4 codes = ((bits >> 8u) & 0x80u) | ((bits >> (10u - mantissa_bits)) & 0x7Fu);
        return codes.x | (codes.y << 8u) | (codes.z << 16u) | (codes.w << 24u);
    }
    float e4m3_to_f32(uint code) { return float(as_type<half>(ushort(fp8_to_f16_bits(code, 3u)))) * 256.0f; }
    float e3m4_to_f32(uint code) { return float(as_type<half>(ushort(fp8_to_f16_bits(code, 4u)))) * 4096.0f; }
    uint f32_to_e4m3(float value) { return f16_bits_to_fp8(as_type<ushort>(half(value * 0.00390625f)), 3u); }
    uint f32_to_e3m4(float value) { return f16_bits_to_fp8(as_type<ushort>(half(value * 0.000244140625f)), 4u); }
    float chomp_load_e4m3(Chomp chomp, uint index) { return e4m3_to_f32(chomp_load_u8(chomp, index)); }
    float chomp_load_e3m4(Chomp chomp, uint index) { return e3m4_to_f32(chomp_load_u8(chomp, index)); }
    void chomp_store_e4m3(Chomp chomp, uint index, float value) { chomp_store_u8(chomp, index, f32_to_e4m3(value)); }
    void chomp_store_e3m4(Chomp chomp, uint index, float value) { chomp_store_u8(chomp, index, f32_to_e3m4(value)); }
    float4 chomp_load_f16x4(Chomp chomp, uint index) { return float4(as_type<half4>(chomp_load_u64(chomp, index))); }
    float4 chomp_load_bf16x4(Chomp chomp, uint index) { return as_type<float4>(uint4(as_type<ushort4>(chomp_load_u64(chomp, index))) << 16u); }
    float4 chomp_load_e5m2x4(Chomp chomp, uint index) { return fp8x4_to_f32x4(chomp_load_u32(chomp, index), 2u, 1.0f); }
    float4 chomp_load_e4m3x4(Chomp chomp, uint index) { return fp8x4_to_f32x4(chomp_load_u32(chomp, index), 3u, 256.0f); }
    float4 chomp_load_e3m4x4(Chomp chomp, uint index) { return fp8x4_to_f32x4(chomp_load_u32(chomp, index), 4u, 4096.0f); }
    void chomp_store_f16x4(Chomp chomp, uint index, float4 values) { chomp_store_u64(chomp, index, as_type<ulong>(half4(values))); }
    void chomp_store_bf16x4(Chomp chomp, uint index, float4 values) { chomp_store_u64(chomp, index, as_type<ulong>(ushort4(as_type<uint4>(values) >> 16u))); }
    void chomp_store_e5m2x4(Chomp chomp, uint index, float4 values) {
        uint4 codes = uint4(as_type<ushort4>(half4(values))) >> 8u;
        chomp_store_u32(chomp, index, codes.x | (codes.y << 8u) | (codes.z << 16u) | (codes.w << 24u));
    }
    void chomp_store_e4m3x4(Chomp chomp, uint index, float4 values) { chomp_store_u32(chomp, index, fp8x4_from_f32x4(values, 3u, 0.00390625f)); }
    void chomp_store_e3m4x4(Chomp chomp, uint index, float4 values) { chomp_store_u32(chomp, index, fp8x4_from_f32x4(values, 4u, 0.000244140625f)); }
    half4 fp8x4_to_f16x4(uint word, uint mantissa_bits, half scale) { return half4(fp8x4_to_f32x4(word, mantissa_bits, float(scale))); }
    half4 chomp_load_f32x4_half(Chomp chomp, uint index) { return half4(chomp_load_f32x4(chomp, index)); }
    half4 chomp_load_f16x4_half(Chomp chomp, uint index) { return as_type<half4>(chomp_load_u64(chomp, index)); }
    half4 chomp_load_bf16x4_half(Chomp chomp, uint index) { return half4(chomp_load_bf16x4(chomp, index)); }
    half4 chomp_load_e5m2x4_half(Chomp chomp, uint index) { return half4(chomp_load_e5m2x4(chomp, index)); }
    half4 chomp_load_e4m3x4_half(Chomp chomp, uint index) { return half4(chomp_load_e4m3x4(chomp, index)); }
    half4 chomp_load_e3m4x4_half(Chomp chomp, uint index) { return half4(chomp_load_e3m4x4(chomp, index)); }
    Chomp vulkan_list() { return chomp_read(vulkan_push.vulkan_table_address); }
    uint vulkan_count() { return gl_NumWorkGroups.y; }
    Chomp vulkan_chomp(uint index) { return chomp_read(chomp_address(vulkan_list()) + ulong(index) * 4ul); }
    uint vulkan_index() { return gl_WorkGroupID.y; }
    Chomp vulkan_job_array() { return chomp_read(vulkan_push.vulkan_table_address); }
    Chomp vulkan_job(uint index) { return chomp_read(chomp_address(vulkan_job_array()) + ulong(index) * 32ul); }
    uint vulkan_lane() { return gl_GlobalInvocationID.x; }
    uint vulkan_part() { return gl_GlobalInvocationID.y; }
    uint vulkan_job_index() { return gl_GlobalInvocationID.z; }
    uint vulkan_job_count() { return gl_NumWorkGroups.z; }
)metal";
inline constexpr std::string_view METAL_SHADER_ENTRY = R"metal(
};
kernel void alligator_entry(constant KernelPush& push [[buffer(0)]],
    uint3 group [[threadgroup_position_in_grid]], uint3 grid [[threadgroups_per_grid]],
    uint3 local [[thread_position_in_threadgroup]], uint local_index [[thread_index_in_threadgroup]],
    uint3 global [[thread_position_in_grid]]) {
    AlligatorProgram program{push, group, grid, local, local_index, global};
)metal";
} // namespace buffetalligator
