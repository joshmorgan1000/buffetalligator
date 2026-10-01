#pragma once
/** --------------------------------------------------------------------------------------------------------- Metal Shader Prelude
 * @file prelude.hpp
 * @brief Defines native MSL Slice methods within each invocation's implicit address context.
 */
#include <string_view>

namespace buffetalligator {
inline constexpr std::string_view METAL_SHADER_CORE = R"metal(
#include <metal_stdlib>
using namespace metal;
struct KernelPush { ulong vulkan_table_address; ulong vulkan_pool_address; };
struct Slice { uint id; };
struct GPUBufRef { ulong address; uint size; uint offset; };
struct AlligatorProgram {
    KernelPush vulkan_push;
    uint3 gl_WorkGroupID;
    uint3 gl_NumWorkGroups;
    uint3 gl_LocalInvocationID;
    uint gl_LocalInvocationIndex;
    uint3 gl_GlobalInvocationID;
    Slice gpu_slice(uint index) { return Slice{index}; }
    ulong slice_table(Slice slice) {
        return reinterpret_cast<device const ulong*>(vulkan_push.vulkan_pool_address)[(slice.id >> 3u) & 63u];
    }
    GPUBufRef slice_ref(Slice slice) {
        return reinterpret_cast<device const GPUBufRef*>(slice_table(slice))[slice.id >> 9u];
    }
    ulong slice_address(Slice slice) {
        GPUBufRef record = slice_ref(slice);
        return record.address + (ulong(record.offset) << 6u);
    }
    bool slice_is_null(Slice slice) { return slice.id == 0xFFFFFFFFu; }
    ulong slice_size(Slice slice) { return ulong(slice_ref(slice).size) << 6u; }
    Slice slice_read(ulong address) { return gpu_slice(*reinterpret_cast<device const uint*>(address)); }
    ulong gpu_slice_address(uint index) { return slice_address(gpu_slice(index)); }
    ulong gpu_slice_size(uint index) { return slice_size(gpu_slice(index)); }
    uint slice_load_u32(Slice slice, uint index) { return reinterpret_cast<device const uint*>(slice_address(slice))[index]; }
    int slice_load_i32(Slice slice, uint index) { return reinterpret_cast<device const int*>(slice_address(slice))[index]; }
    float slice_load_f32(Slice slice, uint index) { return reinterpret_cast<device const float*>(slice_address(slice))[index]; }
    ulong slice_load_u64(Slice slice, uint index) { return reinterpret_cast<device const ulong*>(slice_address(slice))[index]; }
    long slice_load_i64(Slice slice, uint index) { return reinterpret_cast<device const long*>(slice_address(slice))[index]; }
    float4 slice_load_f32x4(Slice slice, uint index) { return reinterpret_cast<device const float4*>(slice_address(slice))[index]; }
    uint4 slice_load_u32x4(Slice slice, uint index) { return reinterpret_cast<device const uint4*>(slice_address(slice))[index]; }
    int4 slice_load_i32x4(Slice slice, uint index) { return reinterpret_cast<device const int4*>(slice_address(slice))[index]; }
    void slice_store_u32(Slice slice, uint index, uint value) { reinterpret_cast<device uint*>(slice_address(slice))[index] = value; }
    void slice_store_i32(Slice slice, uint index, int value) { reinterpret_cast<device int*>(slice_address(slice))[index] = value; }
    void slice_store_f32(Slice slice, uint index, float value) { reinterpret_cast<device float*>(slice_address(slice))[index] = value; }
    void slice_store_u64(Slice slice, uint index, ulong value) { reinterpret_cast<device ulong*>(slice_address(slice))[index] = value; }
    void slice_store_i64(Slice slice, uint index, long value) { reinterpret_cast<device long*>(slice_address(slice))[index] = value; }
    void slice_store_f32x4(Slice slice, uint index, float4 value) { reinterpret_cast<device float4*>(slice_address(slice))[index] = value; }
    void slice_store_u32x4(Slice slice, uint index, uint4 value) { reinterpret_cast<device uint4*>(slice_address(slice))[index] = value; }
    void slice_store_i32x4(Slice slice, uint index, int4 value) { reinterpret_cast<device int4*>(slice_address(slice))[index] = value; }
    uint slice_load_u16(Slice slice, uint index) { return (slice_load_u32(slice, index >> 1u) >> ((index & 1u) * 16u)) & 0xFFFFu; }
    int slice_load_i16(Slice slice, uint index) { return int(short(slice_load_u16(slice, index))); }
    uint slice_load_u8(Slice slice, uint index) { return (slice_load_u32(slice, index >> 2u) >> ((index & 3u) * 8u)) & 0xFFu; }
    int slice_load_i8(Slice slice, uint index) { return int(char(slice_load_u8(slice, index))); }
    float slice_load_f16(Slice slice, uint index) { return float(as_type<half>(ushort(slice_load_u16(slice, index)))); }
    float slice_load_bf16(Slice slice, uint index) { return as_type<float>(slice_load_u16(slice, index) << 16u); }
    float slice_load_e5m2(Slice slice, uint index) { return float(as_type<half>(ushort(slice_load_u8(slice, index) << 8u))); }
    void slice_store_u16(Slice slice, uint index, uint value) {
        device atomic_uint* word = reinterpret_cast<device atomic_uint*>(slice_address(slice) + ulong((index * 2u) & ~3u));
        uint shift = (index & 1u) * 16u;
        atomic_fetch_and_explicit(word, ~(0xFFFFu << shift), memory_order_relaxed);
        atomic_fetch_or_explicit(word, (value & 0xFFFFu) << shift, memory_order_relaxed);
    }
    void slice_store_u8(Slice slice, uint index, uint value) {
        device atomic_uint* word = reinterpret_cast<device atomic_uint*>(slice_address(slice) + ulong(index & ~3u));
        uint shift = (index & 3u) * 8u;
        atomic_fetch_and_explicit(word, ~(0xFFu << shift), memory_order_relaxed);
        atomic_fetch_or_explicit(word, (value & 0xFFu) << shift, memory_order_relaxed);
    }
    void slice_store_i16(Slice slice, uint index, int value) { slice_store_u16(slice, index, uint(value)); }
    void slice_store_i8(Slice slice, uint index, int value) { slice_store_u8(slice, index, uint(value)); }
    void slice_store_f16(Slice slice, uint index, float value) { slice_store_u16(slice, index, as_type<ushort>(half(value))); }
    void slice_store_bf16(Slice slice, uint index, float value) { slice_store_u16(slice, index, as_type<uint>(value) >> 16u); }
    void slice_store_e5m2(Slice slice, uint index, float value) { slice_store_u8(slice, index, as_type<ushort>(half(value)) >> 8u); }
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
    float slice_load_e4m3(Slice slice, uint index) { return e4m3_to_f32(slice_load_u8(slice, index)); }
    float slice_load_e3m4(Slice slice, uint index) { return e3m4_to_f32(slice_load_u8(slice, index)); }
    void slice_store_e4m3(Slice slice, uint index, float value) { slice_store_u8(slice, index, f32_to_e4m3(value)); }
    void slice_store_e3m4(Slice slice, uint index, float value) { slice_store_u8(slice, index, f32_to_e3m4(value)); }
    float4 slice_load_f16x4(Slice slice, uint index) { return float4(as_type<half4>(slice_load_u64(slice, index))); }
    float4 slice_load_bf16x4(Slice slice, uint index) { return as_type<float4>(uint4(as_type<ushort4>(slice_load_u64(slice, index))) << 16u); }
    float4 slice_load_e5m2x4(Slice slice, uint index) { return fp8x4_to_f32x4(slice_load_u32(slice, index), 2u, 1.0f); }
    float4 slice_load_e4m3x4(Slice slice, uint index) { return fp8x4_to_f32x4(slice_load_u32(slice, index), 3u, 256.0f); }
    float4 slice_load_e3m4x4(Slice slice, uint index) { return fp8x4_to_f32x4(slice_load_u32(slice, index), 4u, 4096.0f); }
    void slice_store_f16x4(Slice slice, uint index, float4 values) { slice_store_u64(slice, index, as_type<ulong>(half4(values))); }
    void slice_store_bf16x4(Slice slice, uint index, float4 values) { slice_store_u64(slice, index, as_type<ulong>(ushort4(as_type<uint4>(values) >> 16u))); }
    void slice_store_e5m2x4(Slice slice, uint index, float4 values) {
        uint4 codes = uint4(as_type<ushort4>(half4(values))) >> 8u;
        slice_store_u32(slice, index, codes.x | (codes.y << 8u) | (codes.z << 16u) | (codes.w << 24u));
    }
    void slice_store_e4m3x4(Slice slice, uint index, float4 values) { slice_store_u32(slice, index, fp8x4_from_f32x4(values, 3u, 0.00390625f)); }
    void slice_store_e3m4x4(Slice slice, uint index, float4 values) { slice_store_u32(slice, index, fp8x4_from_f32x4(values, 4u, 0.000244140625f)); }
    half4 fp8x4_to_f16x4(uint word, uint mantissa_bits, half scale) { return half4(fp8x4_to_f32x4(word, mantissa_bits, float(scale))); }
    half4 slice_load_f32x4_half(Slice slice, uint index) { return half4(slice_load_f32x4(slice, index)); }
    half4 slice_load_f16x4_half(Slice slice, uint index) { return as_type<half4>(slice_load_u64(slice, index)); }
    half4 slice_load_bf16x4_half(Slice slice, uint index) { return half4(slice_load_bf16x4(slice, index)); }
    half4 slice_load_e5m2x4_half(Slice slice, uint index) { return half4(slice_load_e5m2x4(slice, index)); }
    half4 slice_load_e4m3x4_half(Slice slice, uint index) { return half4(slice_load_e4m3x4(slice, index)); }
    half4 slice_load_e3m4x4_half(Slice slice, uint index) { return half4(slice_load_e3m4x4(slice, index)); }
    Slice vulkan_list() { return slice_read(vulkan_push.vulkan_table_address); }
    uint vulkan_count() { return gl_NumWorkGroups.y; }
    Slice vulkan_slice(uint index) { return slice_read(slice_address(vulkan_list()) + ulong(index) * 4ul); }
    uint vulkan_index() { return gl_WorkGroupID.y; }
    Slice vulkan_job_array() { return slice_read(vulkan_push.vulkan_table_address); }
    Slice vulkan_job(uint index) { return slice_read(slice_address(vulkan_job_array()) + ulong(index) * 32ul); }
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
