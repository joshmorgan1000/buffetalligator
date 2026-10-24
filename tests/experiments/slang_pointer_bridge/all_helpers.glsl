/** --------------------------------------------------------------------------------------------------------- Helper Coverage
 * @brief Keeps every production typed Chomp helper reachable during CUDA source emission.
 */
void alligator_main(Chomp payload) {
    if (gl_LocalInvocationIndex != 0u) return;
    Chomp nested = chomp_read(chomp_address(payload));
    if (chomp_is_null(nested)) return;
    Chomp selected = gpu_chomp(nested.id);
    chomp_store_u64(payload, 1u, gpu_chomp_address(selected.id));
    chomp_store_u64(payload, 2u, gpu_chomp_size(selected.id));
    chomp_store_u32(selected, 0u, chomp_load_u32(selected, 0u));
    chomp_store_i32(selected, 1u, chomp_load_i32(selected, 1u));
    chomp_store_f32(selected, 2u, chomp_load_f32(selected, 2u));
    chomp_store_u64(selected, 2u, chomp_load_u64(selected, 2u));
    chomp_store_i64(selected, 3u, chomp_load_i64(selected, 3u));
    chomp_store_f32x4(selected, 2u, chomp_load_f32x4(selected, 2u));
    chomp_store_u32x4(selected, 3u, chomp_load_u32x4(selected, 3u));
    chomp_store_i32x4(selected, 4u, chomp_load_i32x4(selected, 4u));
    chomp_store_u16(selected, 40u, chomp_load_u16(selected, 40u));
    chomp_store_i16(selected, 41u, chomp_load_i16(selected, 41u));
    chomp_store_u8(selected, 84u, chomp_load_u8(selected, 84u));
    chomp_store_i8(selected, 85u, chomp_load_i8(selected, 85u));
    chomp_store_f16(selected, 44u, chomp_load_f16(selected, 44u));
    chomp_store_bf16(selected, 45u, chomp_load_bf16(selected, 45u));
    chomp_store_e5m2(selected, 92u, chomp_load_e5m2(selected, 92u));
    chomp_store_e4m3(selected, 93u, chomp_load_e4m3(selected, 93u));
    chomp_store_e3m4(selected, 94u, chomp_load_e3m4(selected, 94u));
    chomp_store_f16x4(selected, 12u, chomp_load_f16x4(selected, 12u));
    chomp_store_bf16x4(selected, 13u, chomp_load_bf16x4(selected, 13u));
    chomp_store_e5m2x4(selected, 28u, chomp_load_e5m2x4(selected, 28u));
    chomp_store_e4m3x4(selected, 29u, chomp_load_e4m3x4(selected, 29u));
    chomp_store_e3m4x4(selected, 30u, chomp_load_e3m4x4(selected, 30u));
#ifdef VULKAN_FLOAT16
    chomp_store_f32x4(payload, 2u, vec4(chomp_load_f32x4_half(selected, 2u)));
    chomp_store_f32x4(payload, 3u, vec4(chomp_load_f16x4_half(selected, 12u)));
    chomp_store_f32x4(payload, 4u, vec4(chomp_load_bf16x4_half(selected, 13u)));
    chomp_store_f32x4(payload, 5u, vec4(chomp_load_e5m2x4_half(selected, 28u)));
    chomp_store_f32x4(payload, 6u, vec4(chomp_load_e4m3x4_half(selected, 29u)));
    chomp_store_f32x4(payload, 7u, vec4(chomp_load_e3m4x4_half(selected, 30u)));
#endif
}
