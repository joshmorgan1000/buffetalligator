#pragma once
/** --------------------------------------------------------------------------------------------------------- Vulkan GLSL Kernel Prelude
 * @file vulkanglsl.hpp
 * @brief GLSL prelude and host-side mirrors for Buffet Alligator's persistent megakernel dispatch.
 *
 * GPU-side model: one push constant (the job table address), one workgroup shape (16, 4, jobs).
 * The table holds one 16-byte descriptor per stage; entry k points at stage k's job array, an
 * array of 32-byte job records (a 16-byte input descriptor plus 16 bytes of host-only handle).
 * The job count arrives as gl_NumWorkGroups.z via indirect dispatch — nothing is pushed per
 * round, and the command buffer never changes.
 *
 * X (16) is the conceptual element lane, or area to shade. This number may change to 8 in the
 *     future so that Y and Z may be scaled up.
 * Y (4) is the author's split, free to mean whatever the kernel wants, as long as the value
 *     is not 1. The shape (N, 1, 1) is prohibited.
 * Z (jobs) is one workgroup per job; the invocation's z is its job index.
 */
#include <alligator.hpp>
#include <logging.hpp>
#include <string>
#include <string_view>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- VULKAN_GLSL_KERNEL_CORE
 * @brief The 16-byte Slice descriptor mirror and every load/store helper over it. Contains no
 * #version line so it can be injected after a user's own, and is idempotent via its guard.
 */
inline constexpr std::string_view VULKAN_GLSL_KERNEL_CORE = R"glsl(#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int32 : require
#ifdef VULKAN_FLOAT16
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#endif
#ifndef VULKAN_GLSL_KERNEL_CORE_INCLUDED
#define VULKAN_GLSL_KERNEL_CORE_INCLUDED 1
// ------------------------------------------------------------------------------------------------- Kernel push block (16 bytes)
// One push block for every kernel family: the per-engine table address (the megakernel's job
// table, the public Shader's parameters list) and the global GPUBufRef pool's device address.
layout(push_constant) uniform AlligatorPush {
    uint64_t vulkan_table_address;  ///< This engine's table: job records or the parameters list
    uint64_t vulkan_pool_address;   ///< The global GPUBufRef table, shared host and device
} alligator_push;
// ------------------------------------------------------------------------------------------------- Slice (16 bytes)
struct Slice {
    uint64_t device_address;  ///< The slab's device address
    uint32_t size;            ///< The claim's size in bytes
    uint32_t offset;          ///< The claim's byte offset within the slab
};
layout(buffer_reference, std430, buffer_reference_align = 8) buffer SliceRef {
    uint64_t device_address;
    uint32_t size;
    uint32_t offset;
};
// ------------------------------------------------------------------------------------------------- GPUBufRef pool (16-byte entries)
// The device half of the index-linked pool; host-side state lives in the CPUBufRef array at the same index.
struct GPUBufRef {
    uint64_t address;      ///< The slice's absolute device address
    uint64_t size;         ///< The slice's size in bytes
};
layout(buffer_reference, std430, buffer_reference_align = 16) buffer GPUBufRefArray {
    GPUBufRef refs[];
};
Slice gpu_slice(uint32_t index) {
    GPUBufRef ref = GPUBufRefArray(alligator_push.vulkan_pool_address).refs[index];
    Slice s;
    s.device_address = ref.address;
    s.size = uint(ref.size);
    s.offset = 0u;
    return s;
}
uint64_t gpu_slice_address(uint32_t index) { return gpu_slice(index).device_address; }
uint gpu_slice_size(uint32_t index) {
    return uint(GPUBufRefArray(alligator_push.vulkan_pool_address).refs[index].size);
}
layout(buffer_reference, std430, buffer_reference_align = 4) buffer U32Array { uint v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer I32Array { int v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer F32Array { float v[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) buffer U64Array { uint64_t v[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) buffer I64Array { int64_t v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer F32x4Array { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer U32x4Array { uvec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer I32x4Array { ivec4 v[]; };
// ------------------------------------------------------------------------------------------------- Slice basics
uint64_t slice_address(Slice s) { return s.device_address + uint64_t(s.offset); }
bool slice_is_null(Slice s) { return s.size == 0u || s.device_address == 0ul; }
uint slice_size(Slice s) { return s.size; }
Slice slice_sub(Slice s, uint64_t offset, uint64_t length) {
    Slice r = s;
    r.offset = s.offset + uint32_t(offset);
    r.size = uint32_t(length);
    return r;
}
Slice slice_read(uint64_t address) {
    SliceRef ref = SliceRef(address);
    Slice s;
    s.device_address = ref.device_address;
    s.size = ref.size;
    s.offset = ref.offset;
    return s;
}
// ------------------------------------------------------------------------------------------------- Word-native loads
uint  slice_load_u32(Slice s, uint index) { return U32Array(slice_address(s)).v[index]; }
int   slice_load_i32(Slice s, uint index) { return I32Array(slice_address(s)).v[index]; }
float slice_load_f32(Slice s, uint index) { return F32Array(slice_address(s)).v[index]; }
uint64_t slice_load_u64(Slice s, uint index) { return U64Array(slice_address(s)).v[index]; }
int64_t  slice_load_i64(Slice s, uint index) { return I64Array(slice_address(s)).v[index]; }
vec4  slice_load_f32x4(Slice s, uint index) { return F32x4Array(slice_address(s)).v[index]; }
uvec4 slice_load_u32x4(Slice s, uint index) { return U32x4Array(slice_address(s)).v[index]; }
ivec4 slice_load_i32x4(Slice s, uint index) { return I32x4Array(slice_address(s)).v[index]; }
// ------------------------------------------------------------------------------------------------- Word-native stores
void slice_store_u32(Slice s, uint index, uint value) { U32Array(slice_address(s)).v[index] = value; }
void slice_store_i32(Slice s, uint index, int value) { I32Array(slice_address(s)).v[index] = value; }
void slice_store_f32(Slice s, uint index, float value) { F32Array(slice_address(s)).v[index] = value; }
void slice_store_u64(Slice s, uint index, uint64_t value) { U64Array(slice_address(s)).v[index] = value; }
void slice_store_i64(Slice s, uint index, int64_t value) { I64Array(slice_address(s)).v[index] = value; }
void slice_store_f32x4(Slice s, uint index, vec4 value) { F32x4Array(slice_address(s)).v[index] = value; }
void slice_store_u32x4(Slice s, uint index, uvec4 value) { U32x4Array(slice_address(s)).v[index] = value; }
void slice_store_i32x4(Slice s, uint index, ivec4 value) { I32x4Array(slice_address(s)).v[index] = value; }
// ------------------------------------------------------------------------------------------------- Sub-word loads (no 8/16-bit storage feature needed)
uint slice_load_u16(Slice s, uint index) {
    uint word = U32Array(slice_address(s) + uint64_t((index * 2u) & ~3u)).v[0];
    return (word >> ((index & 1u) * 16u)) & 0xFFFFu;
}
int slice_load_i16(Slice s, uint index) {
    uint word = U32Array(slice_address(s) + uint64_t((index * 2u) & ~3u)).v[0];
    return bitfieldExtract(int(word), int((index & 1u) * 16u), 16);
}
uint slice_load_u8(Slice s, uint index) {
    uint word = U32Array(slice_address(s) + uint64_t(index & ~3u)).v[0];
    return (word >> ((index & 3u) * 8u)) & 0xFFu;
}
int slice_load_i8(Slice s, uint index) {
    uint word = U32Array(slice_address(s) + uint64_t(index & ~3u)).v[0];
    return bitfieldExtract(int(word), int((index & 3u) * 8u), 8);
}
float slice_load_f16(Slice s, uint index) {
    uint word = U32Array(slice_address(s) + uint64_t((index * 2u) & ~3u)).v[0];
    return unpackHalf2x16(word)[index & 1u];
}
float slice_load_bf16(Slice s, uint index) { return uintBitsToFloat(slice_load_u16(s, index) << 16u); }
float slice_load_e5m2(Slice s, uint index) { return unpackHalf2x16(slice_load_u8(s, index) << 8u).x; }
// ------------------------------------------------------------------------------------------------- Sub-word stores (atomic read-modify-write so neighbouring lanes never clobber each other)
void slice_store_u16(Slice s, uint index, uint value) {
    U32Array words = U32Array(slice_address(s) + uint64_t((index * 2u) & ~3u));
    uint shift = (index & 1u) * 16u;
    atomicAnd(words.v[0], ~(0xFFFFu << shift));
    atomicOr(words.v[0], (value & 0xFFFFu) << shift);
}
void slice_store_u8(Slice s, uint index, uint value) {
    U32Array words = U32Array(slice_address(s) + uint64_t(index & ~3u));
    uint shift = (index & 3u) * 8u;
    atomicAnd(words.v[0], ~(0xFFu << shift));
    atomicOr(words.v[0], (value & 0xFFu) << shift);
}
void slice_store_i16(Slice s, uint index, int value) { slice_store_u16(s, index, uint(value)); }
void slice_store_i8(Slice s, uint index, int value) { slice_store_u8(s, index, uint(value)); }
void slice_store_f16(Slice s, uint index, float value) { slice_store_u16(s, index, packHalf2x16(vec2(value, 0.0)) & 0xFFFFu); }
void slice_store_bf16(Slice s, uint index, float value) { slice_store_u16(s, index, floatBitsToUint(value) >> 16u); }
void slice_store_e5m2(Slice s, uint index, float value) { slice_store_u8(s, index, (packHalf2x16(vec2(value, 0.0)) >> 8u) & 0xFFu); }
// ------------------------------------------------------------------------------------------------- 8-bit float codes: IEEE-style fields, bias 2^(E-1)-1, truncated from fp16 like e5m2 (shift and mask only)
uint fp8_to_f16_bits(uint code, uint mantissa_bits) { return ((code & 0x80u) << 8u) | ((code & 0x7Fu) << (10u - mantissa_bits)); }
uint f16_bits_to_fp8(uint half_bits, uint mantissa_bits) { return ((half_bits >> 8u) & 0x80u) | ((half_bits >> (10u - mantissa_bits)) & 0x7Fu); }
uvec4 fp8x4_codes(uint word) { return uvec4(word & 0xFFu, (word >> 8u) & 0xFFu, (word >> 16u) & 0xFFu, word >> 24u); }
vec4 fp8x4_to_f32x4(uint word, uint mantissa_bits, float scale) {
    uvec4 codes = fp8x4_codes(word);
    uint lo = fp8_to_f16_bits(codes.x, mantissa_bits) | (fp8_to_f16_bits(codes.y, mantissa_bits) << 16u);
    uint hi = fp8_to_f16_bits(codes.z, mantissa_bits) | (fp8_to_f16_bits(codes.w, mantissa_bits) << 16u);
    return vec4(unpackHalf2x16(lo), unpackHalf2x16(hi)) * scale;
}
uint fp8x4_from_f32x4(vec4 values, uint mantissa_bits, float inverse_scale) {
    uint lo = packHalf2x16(values.xy * inverse_scale);
    uint hi = packHalf2x16(values.zw * inverse_scale);
    return f16_bits_to_fp8(lo & 0xFFFFu, mantissa_bits) | (f16_bits_to_fp8(lo >> 16u, mantissa_bits) << 8u)
        | (f16_bits_to_fp8(hi & 0xFFFFu, mantissa_bits) << 16u) | (f16_bits_to_fp8(hi >> 16u, mantissa_bits) << 24u);
}
float e4m3_to_f32(uint code) { return unpackHalf2x16(fp8_to_f16_bits(code, 3u)).x * 256.0; }
float e3m4_to_f32(uint code) { return unpackHalf2x16(fp8_to_f16_bits(code, 4u)).x * 4096.0; }
uint f32_to_e4m3(float value) { return f16_bits_to_fp8(packHalf2x16(vec2(value * 0.00390625, 0.0)) & 0xFFFFu, 3u); }
uint f32_to_e3m4(float value) { return f16_bits_to_fp8(packHalf2x16(vec2(value * 0.000244140625, 0.0)) & 0xFFFFu, 4u); }
float slice_load_e4m3(Slice s, uint index) { return e4m3_to_f32(slice_load_u8(s, index)); }
float slice_load_e3m4(Slice s, uint index) { return e3m4_to_f32(slice_load_u8(s, index)); }
void slice_store_e4m3(Slice s, uint index, float value) { slice_store_u8(s, index, f32_to_e4m3(value)); }
void slice_store_e3m4(Slice s, uint index, float value) { slice_store_u8(s, index, f32_to_e3m4(value)); }
// ------------------------------------------------------------------------------------------------- Four-wide sub-word loads and stores (index4 counts groups of four elements; whole words, no atomics)
vec4 slice_load_f16x4(Slice s, uint index4) {
    uint64_t pair = slice_load_u64(s, index4);
    return vec4(unpackHalf2x16(uint(pair)), unpackHalf2x16(uint(pair >> 32u)));
}
vec4 slice_load_bf16x4(Slice s, uint index4) {
    uint64_t pair = slice_load_u64(s, index4);
    uint lo = uint(pair);
    uint hi = uint(pair >> 32u);
    return vec4(uintBitsToFloat(lo << 16u), uintBitsToFloat(lo & 0xFFFF0000u), uintBitsToFloat(hi << 16u), uintBitsToFloat(hi & 0xFFFF0000u));
}
vec4 slice_load_e5m2x4(Slice s, uint index4) { return fp8x4_to_f32x4(slice_load_u32(s, index4), 2u, 1.0); }
vec4 slice_load_e4m3x4(Slice s, uint index4) { return fp8x4_to_f32x4(slice_load_u32(s, index4), 3u, 256.0); }
vec4 slice_load_e3m4x4(Slice s, uint index4) { return fp8x4_to_f32x4(slice_load_u32(s, index4), 4u, 4096.0); }
void slice_store_f16x4(Slice s, uint index4, vec4 values) {
    slice_store_u64(s, index4, uint64_t(packHalf2x16(values.xy)) | (uint64_t(packHalf2x16(values.zw)) << 32u));
}
void slice_store_bf16x4(Slice s, uint index4, vec4 values) {
    uvec4 bits = floatBitsToUint(values) >> 16u;
    slice_store_u64(s, index4, uint64_t(bits.x | (bits.y << 16u)) | (uint64_t(bits.z | (bits.w << 16u)) << 32u));
}
void slice_store_e5m2x4(Slice s, uint index4, vec4 values) {
    uint lo = packHalf2x16(values.xy);
    uint hi = packHalf2x16(values.zw);
    slice_store_u32(s, index4, ((lo >> 8u) & 0xFFu) | ((lo >> 16u) & 0xFF00u) | ((hi & 0xFF00u) << 8u) | (hi & 0xFF000000u));
}
void slice_store_e4m3x4(Slice s, uint index4, vec4 values) { slice_store_u32(s, index4, fp8x4_from_f32x4(values, 3u, 0.00390625)); }
void slice_store_e3m4x4(Slice s, uint index4, vec4 values) { slice_store_u32(s, index4, fp8x4_from_f32x4(values, 4u, 0.000244140625)); }
#ifdef VULKAN_FLOAT16
// ------------------------------------------------------------------------------------------------- Four-wide loads widened to float16_t (shaderFloat16 devices)
f16vec4 fp8x4_to_f16x4(uint word, uint mantissa_bits, float16_t scale) {
    uvec4 codes = fp8x4_codes(word);
    uint lo = fp8_to_f16_bits(codes.x, mantissa_bits) | (fp8_to_f16_bits(codes.y, mantissa_bits) << 16u);
    uint hi = fp8_to_f16_bits(codes.z, mantissa_bits) | (fp8_to_f16_bits(codes.w, mantissa_bits) << 16u);
    return f16vec4(unpackFloat2x16(lo), unpackFloat2x16(hi)) * scale;
}
f16vec4 slice_load_f32x4_half(Slice s, uint index4) { return f16vec4(slice_load_f32x4(s, index4)); }
f16vec4 slice_load_f16x4_half(Slice s, uint index4) {
    uint64_t pair = slice_load_u64(s, index4);
    return f16vec4(unpackFloat2x16(uint(pair)), unpackFloat2x16(uint(pair >> 32u)));
}
f16vec4 slice_load_bf16x4_half(Slice s, uint index4) { return f16vec4(slice_load_bf16x4(s, index4)); }
f16vec4 slice_load_e5m2x4_half(Slice s, uint index4) { return fp8x4_to_f16x4(slice_load_u32(s, index4), 2u, float16_t(1.0)); }
f16vec4 slice_load_e4m3x4_half(Slice s, uint index4) { return fp8x4_to_f16x4(slice_load_u32(s, index4), 3u, float16_t(256.0)); }
f16vec4 slice_load_e3m4x4_half(Slice s, uint index4) { return fp8x4_to_f16x4(slice_load_u32(s, index4), 4u, float16_t(4096.0)); }
#endif
#endif
)glsl";
/** --------------------------------------------------------------------------------------------------------- VULKAN_GLSL_KERNEL_TAIL
 * @brief The megakernel tail: the job-table push block, the stage slot, the job decoder, and
 * the lane/part/job helpers. The host injects the workgroup shape; the author writes the loop
 * body against these helpers.
 */
inline constexpr std::string_view VULKAN_GLSL_KERNEL_TAIL = R"glsl(
#ifndef VULKAN_STAGE
#define VULKAN_STAGE 0u
#endif
// ------------------------------------------------------------------------------------------------- Job decoding
// The push block (table address + GPUBufRef pool) is declared in the kernel core above.
// A job record is 32 bytes: the input Slice descriptor followed by 16 bytes of host-only handle.
Slice vulkan_job_array() {
    return slice_read(vulkan_push.vulkan_table_address + uint64_t(VULKAN_STAGE) * 16ul);
}
Slice vulkan_job(uint job_index) {
    return slice_read(slice_address(vulkan_job_array()) + uint64_t(job_index) * 32ul);
}
// ------------------------------------------------------------------------------------------------- Shape helpers
uint vulkan_lane() { return gl_GlobalInvocationID.x; }        ///< X: element lane, 0..15
uint vulkan_part() { return gl_GlobalInvocationID.y; }        ///< Y: the author's split, 0..3
uint vulkan_job_index() { return gl_GlobalInvocationID.z; }   ///< Z: this invocation's job
uint vulkan_job_count() { return gl_NumWorkGroups.z; }        ///< Live jobs this dispatch
// ------------------------------------------------------------------------------------------------- Workgroup tree reduction (64 invocations)
// Every invocation must call this (the barriers are workgroup-wide); non-contributing parts
// pass 0.0. The total is valid at lane 0 of part 0.
shared float vulkan_reduce_scratch[64];
float vulkan_group_reduce_sum(float partial) {
    uint index = gl_LocalInvocationIndex;
    vulkan_reduce_scratch[index] = partial;
    barrier();
    if (index < 32u) vulkan_reduce_scratch[index] += vulkan_reduce_scratch[index + 32u];
    barrier();
    if (index < 16u) vulkan_reduce_scratch[index] += vulkan_reduce_scratch[index + 16u];
    barrier();
    if (index < 8u) vulkan_reduce_scratch[index] += vulkan_reduce_scratch[index + 8u];
    barrier();
    if (index < 4u) vulkan_reduce_scratch[index] += vulkan_reduce_scratch[index + 4u];
    barrier();
    if (index < 2u) vulkan_reduce_scratch[index] += vulkan_reduce_scratch[index + 2u];
    barrier();
    if (index == 0u) vulkan_reduce_scratch[0u] += vulkan_reduce_scratch[1u];
    barrier();
    return vulkan_reduce_scratch[0u];
}
)glsl";
/** --------------------------------------------------------------------------------------------------------- VULKAN_GLSL_L2_EXAMPLE
 * @brief Worked example: squared L2 distance, one job per (A, B) vector pair. The parameter
 * blob is { opcode, dimensions, precision, A[], B[] }; the result is one float per job, the
 * sum of squared differences. Lanes stride the dimensions by 16; part 0 contributes, the other
 * parts pass zero so every invocation reaches the reduction's barriers.
 */
inline constexpr std::string_view VULKAN_GLSL_L2_EXAMPLE = R"glsl(
void main() {
    Slice blob = vulkan_job(vulkan_job_index());
    float partial = 0.0;
    if (!slice_is_null(blob)) {
        const uint dimensions = slice_load_u32(blob, 1u);
        // Header is 12 bytes; vectors begin at the 16-byte mark
        Slice a = slice_sub(blob, 16u, uint64_t(dimensions) * 4ul);
        Slice b = slice_sub(blob, 16u + uint64_t(dimensions) * 4ul, uint64_t(dimensions) * 4ul);
        const float contributes = vulkan_part() == 0u ? 1.0 : 0.0;
        for (uint d = vulkan_lane(); d < dimensions; d += 16u) {
            const float diff = slice_load_f32(a, d) - slice_load_f32(b, d);
            partial += contributes * diff * diff;
        }
    }
    const float total = vulkan_group_reduce_sum(partial);
    if (vulkan_lane() == 0u && vulkan_part() == 0u && !slice_is_null(blob)) {
        slice_store_f32(blob, 0u, total);  // Result lands in the blob's opcode slot
    }
}
)glsl";
/** --------------------------------------------------------------------------------------------------------- kernel_shader_source
 * @brief Prepends the kernel prelude (core + tail + enforced workgroup shape) to a kernel body.
 * @param body GLSL defining main(); the shape is injected, not authored.
 * @return The full shader source.
 */
inline std::string kernel_shader_source(std::string_view body) {
    std::string source;
    source.reserve(13 + VULKAN_GLSL_KERNEL_CORE.size() + VULKAN_GLSL_KERNEL_TAIL.size() + body.size());
    source.append("#version 450\n");
    source.append(VULKAN_GLSL_KERNEL_CORE);
    source.append(VULKAN_GLSL_KERNEL_TAIL);
    source.append("layout(local_size_x = 16, local_size_y = 4, local_size_z = 1) in;\n");
    source.append(body);
    return source;
}
} // namespace buffetalligator
