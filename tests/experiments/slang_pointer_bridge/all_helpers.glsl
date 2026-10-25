/** --------------------------------------------------------------------------------------------------------- Helper Coverage
 * @brief Keeps every production typed Slice helper reachable during CUDA source emission.
 */
void alligator_main(Slice payload) {
    if (gl_LocalInvocationIndex != 0u) return;
    Slice nested = slice_read(slice_address(payload));
    if (slice_is_null(nested)) return;
    Slice selected = gpu_slice(nested.id);
    slice_store_u64(payload, 1u, gpu_slice_address(selected.id));
    slice_store_u64(payload, 2u, gpu_slice_size(selected.id));
    slice_store_u32(selected, 0u, slice_load_u32(selected, 0u));
    slice_store_i32(selected, 1u, slice_load_i32(selected, 1u));
    slice_store_f32(selected, 2u, slice_load_f32(selected, 2u));
    slice_store_u64(selected, 2u, slice_load_u64(selected, 2u));
    slice_store_i64(selected, 3u, slice_load_i64(selected, 3u));
    slice_store_f32x4(selected, 2u, slice_load_f32x4(selected, 2u));
    slice_store_u32x4(selected, 3u, slice_load_u32x4(selected, 3u));
    slice_store_i32x4(selected, 4u, slice_load_i32x4(selected, 4u));
    slice_store_u16(selected, 40u, slice_load_u16(selected, 40u));
    slice_store_i16(selected, 41u, slice_load_i16(selected, 41u));
    slice_store_u8(selected, 84u, slice_load_u8(selected, 84u));
    slice_store_i8(selected, 85u, slice_load_i8(selected, 85u));
    slice_store_f16(selected, 44u, slice_load_f16(selected, 44u));
    slice_store_bf16(selected, 45u, slice_load_bf16(selected, 45u));
    slice_store_e5m2(selected, 92u, slice_load_e5m2(selected, 92u));
    slice_store_e4m3(selected, 93u, slice_load_e4m3(selected, 93u));
    slice_store_e3m4(selected, 94u, slice_load_e3m4(selected, 94u));
    slice_store_f16x4(selected, 12u, slice_load_f16x4(selected, 12u));
    slice_store_bf16x4(selected, 13u, slice_load_bf16x4(selected, 13u));
    slice_store_e5m2x4(selected, 28u, slice_load_e5m2x4(selected, 28u));
    slice_store_e4m3x4(selected, 29u, slice_load_e4m3x4(selected, 29u));
    slice_store_e3m4x4(selected, 30u, slice_load_e3m4x4(selected, 30u));
#ifdef VULKAN_FLOAT16
    slice_store_f32x4(payload, 2u, vec4(slice_load_f32x4_half(selected, 2u)));
    slice_store_f32x4(payload, 3u, vec4(slice_load_f16x4_half(selected, 12u)));
    slice_store_f32x4(payload, 4u, vec4(slice_load_bf16x4_half(selected, 13u)));
    slice_store_f32x4(payload, 5u, vec4(slice_load_e5m2x4_half(selected, 28u)));
    slice_store_f32x4(payload, 6u, vec4(slice_load_e4m3x4_half(selected, 29u)));
    slice_store_f32x4(payload, 7u, vec4(slice_load_e3m4x4_half(selected, 30u)));
#endif
}
