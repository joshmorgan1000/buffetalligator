#pragma once
/** --------------------------------------------------------------------------------------------------------- SIMD
 * @file simd.hpp
 * @brief SIMD utility functions and classes for the Buffet Alligator project.
 */
#ifdef ALLIGATOR_SIMD_MODE
#undef ALLIGATOR_SIMD_MODE
#endif
#if (__has_include(<simd/simd.h>)) && !defined(ALLIGATOR_SIMD_NO_APPLE)
    #include <simd/simd.h>
    #define ALLIGATOR_SIMD_MODE 1
#elif defined(__ARM_NEON) || defined(__aarch64__)
    #include <arm_neon.h>
    #define ALLIGATOR_SIMD_MODE 6
#elif defined(__AVX512F__) && defined(__AVX512BW__)
    #include <immintrin.h>
    #define ALLIGATOR_SIMD_MODE 2
#elif defined(__AVX2__)
    #include <immintrin.h>
    #define ALLIGATOR_SIMD_MODE 3
#elif defined(__SSE4_2__)
    #include <nmmintrin.h>
    #define ALLIGATOR_SIMD_MODE 4
#elif (__has_include(<altivec.h>))
    #include <altivec.h>
    #define ALLIGATOR_SIMD_MODE 5
#endif
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace buffetalligator {
/// @brief Dependent false for static_assert inside constexpr-if chains.
template<typename> struct always_false : std::false_type {};
/** --------------------------------------------------------------------------------------------------------- SIMDMisc
 * @struct SIMDMisc
 * @brief Static utility class for SIMD operations
 */
struct SIMDMisc {
    /// @brief Static utility class for SIMD operations, no instances allowed.
    SIMDMisc() = delete;
    /** ------------------------------------------------------------------------------------------- find_id
     * @brief Flat-scan an ID array for a value and return its slot.
     * @param ids Pointer to the array of IDs.
     * @param size Number of elements in the array.
     * @param id ID to search for.
     * @return Slot of the first match, or -1 when absent.
     */
    template<typename T>
        requires std::is_convertible_v<T, int64_t>
    static int64_t find_id(const T* ids, size_t size, int64_t id) {
        size_t i = 0;
#if ALLIGATOR_SIMD_MODE == 1
        if constexpr (std::is_same_v<T, int64_t> || std::is_same_v<T, long long>) {
            const simd_long8 id_v = simd_long8(id);
            #pragma unroll 8
            for (; i + 8 <= size; i += 8) {
                const simd_long8 ids_v = *reinterpret_cast<const simd_packed_long8*>(ids + i);
                if (simd_any(ids_v == id_v)) {
                    #pragma unroll
                    for (int j = 0; j < 8; ++j) {
                        if (ids[i + j] == id) return static_cast<int64_t>(i + j);
                    }
                }
            }
        } else if constexpr (std::is_same_v<T, uint64_t> || std::is_same_v<T, unsigned long long>) {
            const simd_ulong8 id_v = simd_ulong8(static_cast<uint64_t>(id));
            #pragma unroll 8
            for (; i + 8 <= size; i += 8) {
                const simd_ulong8 ids_v = *reinterpret_cast<const simd_packed_ulong8*>(ids + i);
                if (simd_any(ids_v == id_v)) {
                    #pragma unroll
                    for (int j = 0; j < 8; ++j) {
                        if (static_cast<int64_t>(ids[i + j]) == id) return static_cast<int64_t>(i + j);
                    }
                }
            }
        } else if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, int>) {
            const simd_int16 id_v = simd_int16(static_cast<int32_t>(id));
            #pragma unroll 8
            for (; i + 16 <= size; i += 16) {
                const simd_int16 ids_v = *reinterpret_cast<const simd_packed_int16*>(ids + i);
                if (simd_any(ids_v == id_v)) {
                    #pragma unroll
                    for (int j = 0; j < 16; ++j) {
                        if (static_cast<int64_t>(ids[i + j]) == id) return static_cast<int64_t>(i + j);
                    }
                }
            }
        } else if constexpr (std::is_same_v<T, uint32_t> || std::is_same_v<T, unsigned int>) {
            const simd_uint16 id_v = simd_uint16(static_cast<uint32_t>(id));
            #pragma unroll 8
            for (; i + 16 <= size; i += 16) {
                const simd_uint16 ids_v = *reinterpret_cast<const simd_packed_uint16*>(ids + i);
                if (simd_any(ids_v == id_v)) {
                    #pragma unroll
                    for (int j = 0; j < 16; ++j) {
                        if (static_cast<int64_t>(ids[i + j]) == id) return static_cast<int64_t>(i + j);
                    }
                }
            }
        } else if constexpr (std::is_same_v<T, int16_t> || std::is_same_v<T, short>) {
            const simd_short32 id_v = simd_short32(static_cast<int16_t>(id));
            const simd_short8 id_v8 = simd_short8(static_cast<int16_t>(id));
            #pragma unroll 8
            for (; i + 32 <= size; i += 32) {
                const simd_short32 ids_v = *reinterpret_cast<const simd_packed_short32*>(ids + i);
                if (simd_any(ids_v == id_v)) {
                    #pragma unroll
                    for (int j = 0; j < 4; ++j) {
                        const simd_short8 ids_chunk =
                            *reinterpret_cast<const simd_packed_short8*>(ids + i + j * 8);
                        if (simd_any(ids_chunk == id_v8)) {
                            #pragma unroll
                            for (int k = 0; k < 8; ++k) {
                                if (static_cast<int64_t>(ids[i + j * 8 + k]) == id) {
                                    return static_cast<int64_t>(i + j * 8 + k);
                                }
                            }
                        }
                    }
                }
            }
        } else if constexpr (std::is_same_v<T, uint16_t> || std::is_same_v<T, unsigned short>) {
            const simd_ushort32 id_v = simd_ushort32(static_cast<uint16_t>(id));
            const simd_ushort8 id_v8 = simd_ushort8(static_cast<uint16_t>(id));
            #pragma unroll 8
            for (; i + 32 <= size; i += 32) {
                const simd_ushort32 ids_v = *reinterpret_cast<const simd_packed_ushort32*>(ids + i);
                if (simd_any(ids_v == id_v)) {
                    #pragma unroll
                    for (int j = 0; j < 4; ++j) {
                        const simd_ushort8 ids_chunk =
                            *reinterpret_cast<const simd_packed_ushort8*>(ids + i + j * 8);
                        if (simd_any(ids_chunk == id_v8)) {
                            #pragma unroll
                            for (int k = 0; k < 8; ++k) {
                                if (static_cast<int64_t>(ids[i + j * 8 + k]) == id) {
                                    return static_cast<int64_t>(i + j * 8 + k);
                                }
                            }
                        }
                    }
                }
            }
        } else {
            static_assert(always_false<T>::value, "Unsupported type for SIMD search");
        }
#elif ALLIGATOR_SIMD_MODE == 2
        if constexpr (sizeof(T) == 8) {
            const __m512i id_v = _mm512_set1_epi64(id);
            for (; i + 8 <= size; i += 8) {
                const __m512i ids_v = _mm512_loadu_si512(ids + i);
                const __mmask8 mask = _mm512_cmpeq_epi64_mask(ids_v, id_v);
                if (mask != 0) {
                    return static_cast<int64_t>(i + __builtin_ctz(mask));
                }
            }
        } else if constexpr (sizeof(T) == 4) {
            const __m512i id_v = _mm512_set1_epi32(static_cast<int32_t>(id));
            for (; i + 16 <= size; i += 16) {
                const __m512i ids_v = _mm512_loadu_si512(ids + i);
                const __mmask16 mask = _mm512_cmpeq_epi32_mask(ids_v, id_v);
                if (mask != 0) {
                    return static_cast<int64_t>(i + __builtin_ctz(mask));
                }
            }
        } else if constexpr (sizeof(T) == 2) {
            const __m512i id_v = _mm512_set1_epi16(static_cast<int16_t>(id));
            for (; i + 32 <= size; i += 32) {
                const __m512i ids_v = _mm512_loadu_si512(ids + i);
                const __mmask32 mask = _mm512_cmpeq_epi16_mask(ids_v, id_v);
                if (mask != 0) {
                    return static_cast<int64_t>(i + __builtin_ctz(mask));
                }
            }
        } else {
            static_assert(always_false<T>::value, "Unsupported type for SIMD search");
        }
#elif ALLIGATOR_SIMD_MODE == 3
        if constexpr (sizeof(T) == 8) {
            const __m256i id_v = _mm256_set1_epi64x(id);
            for (; i + 4 <= size; i += 4) {
                const __m256i ids_v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ids + i));
                const int mask = _mm256_movemask_epi8(_mm256_cmpeq_epi64(ids_v, id_v));
                if (mask != 0) {
                    return static_cast<int64_t>(i + (__builtin_ctz(mask) >> 3));
                }
            }
        } else if constexpr (sizeof(T) == 4) {
            const __m256i id_v = _mm256_set1_epi32(static_cast<int32_t>(id));
            for (; i + 8 <= size; i += 8) {
                const __m256i ids_v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ids + i));
                const int mask = _mm256_movemask_epi8(_mm256_cmpeq_epi32(ids_v, id_v));
                if (mask != 0) {
                    return static_cast<int64_t>(i + (__builtin_ctz(mask) >> 2));
                }
            }
        } else if constexpr (sizeof(T) == 2) {
            const __m256i id_v = _mm256_set1_epi16(static_cast<int16_t>(id));
            for (; i + 16 <= size; i += 16) {
                const __m256i ids_v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ids + i));
                const int mask = _mm256_movemask_epi8(_mm256_cmpeq_epi16(ids_v, id_v));
                if (mask != 0) {
                    return static_cast<int64_t>(i + (__builtin_ctz(mask) >> 1));
                }
            }
        } else {
            static_assert(always_false<T>::value, "Unsupported type for SIMD search");
        }
#elif ALLIGATOR_SIMD_MODE == 4
        if constexpr (sizeof(T) == 8) {
            const __m128i id_v = _mm_set1_epi64x(id);
            for (; i + 2 <= size; i += 2) {
                const __m128i ids_v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ids + i));
                const int mask = _mm_movemask_epi8(_mm_cmpeq_epi64(ids_v, id_v));
                if (mask != 0) {
                    return static_cast<int64_t>(i + (__builtin_ctz(mask) >> 3));
                }
            }
        } else if constexpr (sizeof(T) == 4) {
            const __m128i id_v = _mm_set1_epi32(static_cast<int32_t>(id));
            for (; i + 4 <= size; i += 4) {
                const __m128i ids_v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ids + i));
                const int mask = _mm_movemask_epi8(_mm_cmpeq_epi32(ids_v, id_v));
                if (mask != 0) {
                    return static_cast<int64_t>(i + (__builtin_ctz(mask) >> 2));
                }
            }
        } else if constexpr (sizeof(T) == 2) {
            const __m128i id_v = _mm_set1_epi16(static_cast<int16_t>(id));
            for (; i + 8 <= size; i += 8) {
                const __m128i ids_v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ids + i));
                const int mask = _mm_movemask_epi8(_mm_cmpeq_epi16(ids_v, id_v));
                if (mask != 0) {
                    return static_cast<int64_t>(i + (__builtin_ctz(mask) >> 1));
                }
            }
        } else {
            static_assert(always_false<T>::value, "Unsupported type for SIMD search");
        }
#elif ALLIGATOR_SIMD_MODE == 5
        if constexpr (sizeof(T) == 8) {
            const __vector signed long long id_v = vec_splats(static_cast<signed long long>(id));
            for (; i + 2 <= size; i += 2) {
                const __vector signed long long ids_v =
                    vec_xl(0, reinterpret_cast<const signed long long*>(ids + i));
                if (vec_any_eq(ids_v, id_v)) {
                    if (static_cast<int64_t>(ids[i]) == id) {
                        return static_cast<int64_t>(i);
                    }
                    return static_cast<int64_t>(i + 1);
                }
            }
        } else if constexpr (sizeof(T) == 4) {
            const __vector signed int id_v = vec_splats(static_cast<signed int>(id));
            for (; i + 4 <= size; i += 4) {
                const __vector signed int ids_v =
                    vec_xl(0, reinterpret_cast<const signed int*>(ids + i));
                if (vec_any_eq(ids_v, id_v)) {
                    for (int j = 0; j < 4; ++j) {
                        if (static_cast<int64_t>(ids[i + j]) == id) {
                            return static_cast<int64_t>(i + j);
                        }
                    }
                }
            }
        } else if constexpr (sizeof(T) == 2) {
            const __vector signed short id_v = vec_splats(static_cast<signed short>(id));
            for (; i + 8 <= size; i += 8) {
                const __vector signed short ids_v =
                    vec_xl(0, reinterpret_cast<const signed short*>(ids + i));
                if (vec_any_eq(ids_v, id_v)) {
                    for (int j = 0; j < 8; ++j) {
                        if (static_cast<int64_t>(ids[i + j]) == id) {
                            return static_cast<int64_t>(i + j);
                        }
                    }
                }
            }
        } else {
            static_assert(always_false<T>::value, "Unsupported type for SIMD search");
        }
#elif ALLIGATOR_SIMD_MODE == 6
        if constexpr (sizeof(T) == 8) {
            const uint64x2_t id_v = vdupq_n_u64(static_cast<uint64_t>(id));
            for (; i + 2 <= size; i += 2) {
                const uint64x2_t ids_v = vld1q_u64(reinterpret_cast<const uint64_t*>(ids + i));
                const uint64x2_t hits = vceqq_u64(ids_v, id_v);
                if (vgetq_lane_u64(hits, 0) != 0) return static_cast<int64_t>(i);
                if (vgetq_lane_u64(hits, 1) != 0) return static_cast<int64_t>(i + 1);
            }
        } else if constexpr (sizeof(T) == 4) {
            const uint32x4_t id_v = vdupq_n_u32(static_cast<uint32_t>(id));
            for (; i + 4 <= size; i += 4) {
                const uint32x4_t ids_v = vld1q_u32(reinterpret_cast<const uint32_t*>(ids + i));
                if (vmaxvq_u32(vceqq_u32(ids_v, id_v)) != 0) {
                    for (int j = 0; j < 4; ++j) {
                        if (static_cast<int64_t>(ids[i + j]) == id) {
                            return static_cast<int64_t>(i + j);
                        }
                    }
                }
            }
        } else if constexpr (sizeof(T) == 2) {
            const uint16x8_t id_v = vdupq_n_u16(static_cast<uint16_t>(id));
            for (; i + 8 <= size; i += 8) {
                const uint16x8_t ids_v = vld1q_u16(reinterpret_cast<const uint16_t*>(ids + i));
                if (vmaxvq_u16(vceqq_u16(ids_v, id_v)) != 0) {
                    for (int j = 0; j < 8; ++j) {
                        if (static_cast<int64_t>(ids[i + j]) == id) {
                            return static_cast<int64_t>(i + j);
                        }
                    }
                }
            }
        } else {
            static_assert(always_false<T>::value, "Unsupported type for SIMD search");
        }
#endif
        // Check remaining elements
        for (; i < size; ++i) {
            if (static_cast<int64_t>(ids[i]) == id) {
                return static_cast<int64_t>(i);
            }
        }
        return -1;
    }
    /** ------------------------------------------------------------------------------------------- Max Index
     * @brief Flat-scan unsigned 64-bit words for the largest, also reporting the second largest.
     * @param words Pointer to the words.
     * @param size Number of words, at least one.
     * @param max Receives the largest word.
     * @param runner_up Receives the second largest word, zero when there is none.
     * @return Slot of the largest word.
     */
    static size_t max_index(
        const uint64_t* words,
        size_t size,
        uint64_t& max,
        uint64_t& runner_up
    ) {
        max = 0;
        runner_up = 0;
        size_t index = 0;
        size_t i = 0;
        if (size < 16) {
            for (; i < size; ++i) merge_max(words[i], 0, i, max, runner_up, index);
            return index;
        }
#if ALLIGATOR_SIMD_MODE == 1
        simd_ulong8 lane_max = 0;
        simd_ulong8 lane_second = 0;
        simd_ulong8 lane_base = 0;
        for (; i + 8 <= size; i += 8) {
            const simd_ulong8 v = *reinterpret_cast<const simd_packed_ulong8*>(words + i);
            const simd_long8 above = v > lane_max;
            lane_second = simd_bitselect(simd_max(lane_second, v), lane_max, above);
            lane_base = simd_bitselect(lane_base, simd_ulong8(static_cast<uint64_t>(i)), above);
            lane_max = simd_bitselect(lane_max, v, above);
        }
        max = reduce_max8(lane_max);
        size_t lane = 0;
        while (lane_max[lane] != max) ++lane;
        index = lane_base[lane] + lane;
        lane_max[lane] = 0;
        const uint64_t demoted = reduce_max8(lane_max);
        const uint64_t second = reduce_max8(lane_second);
        runner_up = demoted > second ? demoted : second;
#elif ALLIGATOR_SIMD_MODE == 2
        __m512i lane_max = _mm512_setzero_si512();
        __m512i lane_second = _mm512_setzero_si512();
        __m512i lane_base = _mm512_setzero_si512();
        for (; i + 8 <= size; i += 8) {
            const __m512i v = _mm512_loadu_si512(words + i);
            const __mmask8 above = _mm512_cmpgt_epu64_mask(v, lane_max);
            const __m512i base = _mm512_set1_epi64(static_cast<long long>(i));
            lane_second = _mm512_mask_blend_epi64(
                above, _mm512_max_epu64(lane_second, v), lane_max);
            lane_base = _mm512_mask_blend_epi64(above, lane_base, base);
            lane_max = _mm512_mask_blend_epi64(above, lane_max, v);
        }
        uint64_t maxes[8];
        uint64_t seconds[8];
        uint64_t bases[8];
        _mm512_storeu_si512(maxes, lane_max);
        _mm512_storeu_si512(seconds, lane_second);
        _mm512_storeu_si512(bases, lane_base);
        for (size_t lane = 0; lane < 8; ++lane) {
            merge_max(maxes[lane], seconds[lane], bases[lane] + lane, max, runner_up, index);
        }
#elif ALLIGATOR_SIMD_MODE == 3
        const __m256i sign = _mm256_set1_epi64x(static_cast<long long>(0x8000000000000000ull));
        __m256i lane_max = _mm256_setzero_si256();
        __m256i lane_second = _mm256_setzero_si256();
        __m256i lane_base = _mm256_setzero_si256();
        for (; i + 4 <= size; i += 4) {
            const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(words + i));
            const __m256i v_signed = _mm256_xor_si256(v, sign);
            const __m256i above = _mm256_cmpgt_epi64(v_signed, _mm256_xor_si256(lane_max, sign));
            const __m256i above_second = _mm256_cmpgt_epi64(
                v_signed, _mm256_xor_si256(lane_second, sign));
            const __m256i raised = _mm256_blendv_epi8(lane_second, v, above_second);
            const __m256i base = _mm256_set1_epi64x(static_cast<long long>(i));
            lane_second = _mm256_blendv_epi8(raised, lane_max, above);
            lane_base = _mm256_blendv_epi8(lane_base, base, above);
            lane_max = _mm256_blendv_epi8(lane_max, v, above);
        }
        uint64_t maxes[4];
        uint64_t seconds[4];
        uint64_t bases[4];
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(maxes), lane_max);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(seconds), lane_second);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(bases), lane_base);
        for (size_t lane = 0; lane < 4; ++lane) {
            merge_max(maxes[lane], seconds[lane], bases[lane] + lane, max, runner_up, index);
        }
#elif ALLIGATOR_SIMD_MODE == 4
        const __m128i sign = _mm_set1_epi64x(static_cast<long long>(0x8000000000000000ull));
        __m128i lane_max = _mm_setzero_si128();
        __m128i lane_second = _mm_setzero_si128();
        __m128i lane_base = _mm_setzero_si128();
        for (; i + 2 <= size; i += 2) {
            const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(words + i));
            const __m128i v_signed = _mm_xor_si128(v, sign);
            const __m128i above = _mm_cmpgt_epi64(v_signed, _mm_xor_si128(lane_max, sign));
            const __m128i above_second = _mm_cmpgt_epi64(
                v_signed, _mm_xor_si128(lane_second, sign));
            const __m128i raised = _mm_blendv_epi8(lane_second, v, above_second);
            const __m128i base = _mm_set1_epi64x(static_cast<long long>(i));
            lane_second = _mm_blendv_epi8(raised, lane_max, above);
            lane_base = _mm_blendv_epi8(lane_base, base, above);
            lane_max = _mm_blendv_epi8(lane_max, v, above);
        }
        uint64_t maxes[2];
        uint64_t seconds[2];
        uint64_t bases[2];
        _mm_storeu_si128(reinterpret_cast<__m128i*>(maxes), lane_max);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(seconds), lane_second);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(bases), lane_base);
        for (size_t lane = 0; lane < 2; ++lane) {
            merge_max(maxes[lane], seconds[lane], bases[lane] + lane, max, runner_up, index);
        }
#elif ALLIGATOR_SIMD_MODE == 5
        __vector unsigned long long lane_max = vec_splats(0ull);
        __vector unsigned long long lane_second = vec_splats(0ull);
        __vector unsigned long long lane_base = vec_splats(0ull);
        for (; i + 2 <= size; i += 2) {
            const __vector unsigned long long v =
                vec_xl(0, reinterpret_cast<const unsigned long long*>(words + i));
            const __vector bool long long above = vec_cmpgt(v, lane_max);
            lane_second = vec_sel(vec_max(lane_second, v), lane_max, above);
            lane_base = vec_sel(lane_base, vec_splats(static_cast<unsigned long long>(i)), above);
            lane_max = vec_sel(lane_max, v, above);
        }
        for (size_t lane = 0; lane < 2; ++lane) {
            merge_max(lane_max[lane], lane_second[lane], lane_base[lane] + lane,
                max, runner_up, index);
        }
#elif ALLIGATOR_SIMD_MODE == 6
        uint64x2_t lane_max[2] = {vdupq_n_u64(0), vdupq_n_u64(0)};
        uint64x2_t lane_second[2] = {vdupq_n_u64(0), vdupq_n_u64(0)};
        uint64x2_t lane_base[2] = {vdupq_n_u64(0), vdupq_n_u64(0)};
        for (; i + 4 <= size; i += 4) {
            const uint64x2_t base = vdupq_n_u64(static_cast<uint64_t>(i));
            for (size_t half = 0; half < 2; ++half) {
                const uint64x2_t v = vld1q_u64(words + i + half * 2);
                const uint64x2_t above = vcgtq_u64(v, lane_max[half]);
                const uint64x2_t above_second = vcgtq_u64(v, lane_second[half]);
                const uint64x2_t raised = vbslq_u64(above_second, v, lane_second[half]);
                lane_second[half] = vbslq_u64(above, lane_max[half], raised);
                lane_base[half] = vbslq_u64(above, base, lane_base[half]);
                lane_max[half] = vbslq_u64(above, v, lane_max[half]);
            }
        }
        uint64_t maxes[4];
        uint64_t seconds[4];
        uint64_t bases[4];
        for (size_t half = 0; half < 2; ++half) {
            vst1q_u64(maxes + half * 2, lane_max[half]);
            vst1q_u64(seconds + half * 2, lane_second[half]);
            vst1q_u64(bases + half * 2, lane_base[half]);
        }
        for (size_t lane = 0; lane < 4; ++lane) {
            merge_max(maxes[lane], seconds[lane], bases[lane] + lane, max, runner_up, index);
        }
#endif
        for (; i < size; ++i) merge_max(words[i], 0, i, max, runner_up, index);
        return index;
    }
    /** ------------------------------------------------------------------------------------------- Min Index
     * @brief Flat-scan unsigned 64-bit words for the smallest.
     * @param words Pointer to the words.
     * @param size Number of words, at least one.
     * @param min Receives the smallest word.
     * @return Slot of the smallest word.
     */
    static size_t min_index(const uint64_t* words, size_t size, uint64_t& min) {
        min = ~uint64_t(0);
        size_t index = 0;
        size_t i = 0;
        if (size < 16) {
            for (; i < size; ++i) merge_min(words[i], i, min, index);
            return index;
        }
#if ALLIGATOR_SIMD_MODE == 1
        simd_ulong8 lane_min = simd_ulong8(~uint64_t(0));
        simd_ulong8 lane_base = 0;
        for (; i + 8 <= size; i += 8) {
            const simd_ulong8 v = *reinterpret_cast<const simd_packed_ulong8*>(words + i);
            const simd_long8 below = v < lane_min;
            lane_base = simd_bitselect(lane_base, simd_ulong8(static_cast<uint64_t>(i)), below);
            lane_min = simd_bitselect(lane_min, v, below);
        }
        min = reduce_min8(lane_min);
        size_t lane = 0;
        while (lane_min[lane] != min) ++lane;
        index = lane_base[lane] + lane;
#elif ALLIGATOR_SIMD_MODE == 2
        __m512i lane_min = _mm512_set1_epi64(-1);
        __m512i lane_base = _mm512_setzero_si512();
        for (; i + 8 <= size; i += 8) {
            const __m512i v = _mm512_loadu_si512(words + i);
            const __mmask8 below = _mm512_cmplt_epu64_mask(v, lane_min);
            const __m512i base = _mm512_set1_epi64(static_cast<long long>(i));
            lane_base = _mm512_mask_blend_epi64(below, lane_base, base);
            lane_min = _mm512_mask_blend_epi64(below, lane_min, v);
        }
        uint64_t mins[8];
        uint64_t bases[8];
        _mm512_storeu_si512(mins, lane_min);
        _mm512_storeu_si512(bases, lane_base);
        for (size_t lane = 0; lane < 8; ++lane) {
            merge_min(mins[lane], bases[lane] + lane, min, index);
        }
#elif ALLIGATOR_SIMD_MODE == 3
        const __m256i sign = _mm256_set1_epi64x(static_cast<long long>(0x8000000000000000ull));
        __m256i lane_min = _mm256_set1_epi64x(-1);
        __m256i lane_base = _mm256_setzero_si256();
        for (; i + 4 <= size; i += 4) {
            const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(words + i));
            const __m256i below = _mm256_cmpgt_epi64(
                _mm256_xor_si256(lane_min, sign), _mm256_xor_si256(v, sign));
            const __m256i base = _mm256_set1_epi64x(static_cast<long long>(i));
            lane_base = _mm256_blendv_epi8(lane_base, base, below);
            lane_min = _mm256_blendv_epi8(lane_min, v, below);
        }
        uint64_t mins[4];
        uint64_t bases[4];
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(mins), lane_min);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(bases), lane_base);
        for (size_t lane = 0; lane < 4; ++lane) {
            merge_min(mins[lane], bases[lane] + lane, min, index);
        }
#elif ALLIGATOR_SIMD_MODE == 4
        const __m128i sign = _mm_set1_epi64x(static_cast<long long>(0x8000000000000000ull));
        __m128i lane_min = _mm_set1_epi64x(-1);
        __m128i lane_base = _mm_setzero_si128();
        for (; i + 2 <= size; i += 2) {
            const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(words + i));
            const __m128i below = _mm_cmpgt_epi64(
                _mm_xor_si128(lane_min, sign), _mm_xor_si128(v, sign));
            const __m128i base = _mm_set1_epi64x(static_cast<long long>(i));
            lane_base = _mm_blendv_epi8(lane_base, base, below);
            lane_min = _mm_blendv_epi8(lane_min, v, below);
        }
        uint64_t mins[2];
        uint64_t bases[2];
        _mm_storeu_si128(reinterpret_cast<__m128i*>(mins), lane_min);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(bases), lane_base);
        for (size_t lane = 0; lane < 2; ++lane) {
            merge_min(mins[lane], bases[lane] + lane, min, index);
        }
#elif ALLIGATOR_SIMD_MODE == 5
        __vector unsigned long long lane_min = vec_splats(~0ull);
        __vector unsigned long long lane_base = vec_splats(0ull);
        for (; i + 2 <= size; i += 2) {
            const __vector unsigned long long v =
                vec_xl(0, reinterpret_cast<const unsigned long long*>(words + i));
            const __vector bool long long below = vec_cmplt(v, lane_min);
            lane_base = vec_sel(lane_base, vec_splats(static_cast<unsigned long long>(i)), below);
            lane_min = vec_sel(lane_min, v, below);
        }
        for (size_t lane = 0; lane < 2; ++lane) {
            merge_min(lane_min[lane], lane_base[lane] + lane, min, index);
        }
#elif ALLIGATOR_SIMD_MODE == 6
        uint64x2_t lane_min[2] = {vdupq_n_u64(~uint64_t(0)), vdupq_n_u64(~uint64_t(0))};
        uint64x2_t lane_base[2] = {vdupq_n_u64(0), vdupq_n_u64(0)};
        for (; i + 4 <= size; i += 4) {
            const uint64x2_t base = vdupq_n_u64(static_cast<uint64_t>(i));
            for (size_t half = 0; half < 2; ++half) {
                const uint64x2_t v = vld1q_u64(words + i + half * 2);
                const uint64x2_t below = vcltq_u64(v, lane_min[half]);
                lane_base[half] = vbslq_u64(below, base, lane_base[half]);
                lane_min[half] = vbslq_u64(below, v, lane_min[half]);
            }
        }
        uint64_t mins[4];
        uint64_t bases[4];
        for (size_t half = 0; half < 2; ++half) {
            vst1q_u64(mins + half * 2, lane_min[half]);
            vst1q_u64(bases + half * 2, lane_base[half]);
        }
        for (size_t lane = 0; lane < 4; ++lane) {
            merge_min(mins[lane], bases[lane] + lane, min, index);
        }
#endif
        for (; i < size; ++i) merge_min(words[i], i, min, index);
        return index;
    }
    /** ------------------------------------------------------------------------------------------- Min Index With Runner-Up
     * @brief Flat-scan unsigned 64-bit words for the smallest, also reporting the second smallest.
     * @param words Pointer to the words.
     * @param size Number of words, at least one.
     * @param min Receives the smallest word.
     * @param runner_up Receives the second smallest word, all ones when there is none.
     * @return Slot of the smallest word.
     */
    static size_t min_index(
        const uint64_t* words,
        size_t size,
        uint64_t& min,
        uint64_t& runner_up
    ) {
        min = ~uint64_t(0);
        runner_up = ~uint64_t(0);
        size_t index = 0;
        size_t i = 0;
        if (size < 16) {
            for (; i < size; ++i) merge_min(words[i], ~uint64_t(0), i, min, runner_up, index);
            return index;
        }
#if ALLIGATOR_SIMD_MODE == 1
        simd_ulong8 lane_min = simd_ulong8(~uint64_t(0));
        simd_ulong8 lane_second = simd_ulong8(~uint64_t(0));
        simd_ulong8 lane_base = 0;
        for (; i + 8 <= size; i += 8) {
            const simd_ulong8 v = *reinterpret_cast<const simd_packed_ulong8*>(words + i);
            const simd_long8 below = v < lane_min;
            lane_second = simd_bitselect(simd_min(lane_second, v), lane_min, below);
            lane_base = simd_bitselect(lane_base, simd_ulong8(static_cast<uint64_t>(i)), below);
            lane_min = simd_bitselect(lane_min, v, below);
        }
        min = reduce_min8(lane_min);
        size_t lane = 0;
        while (lane_min[lane] != min) ++lane;
        index = lane_base[lane] + lane;
        lane_min[lane] = ~uint64_t(0);
        const uint64_t promoted = reduce_min8(lane_min);
        const uint64_t second = reduce_min8(lane_second);
        runner_up = promoted < second ? promoted : second;
#elif ALLIGATOR_SIMD_MODE == 2
        __m512i lane_min = _mm512_set1_epi64(-1);
        __m512i lane_second = _mm512_set1_epi64(-1);
        __m512i lane_base = _mm512_setzero_si512();
        for (; i + 8 <= size; i += 8) {
            const __m512i v = _mm512_loadu_si512(words + i);
            const __mmask8 below = _mm512_cmplt_epu64_mask(v, lane_min);
            const __m512i base = _mm512_set1_epi64(static_cast<long long>(i));
            lane_second = _mm512_mask_blend_epi64(
                below, _mm512_min_epu64(lane_second, v), lane_min);
            lane_base = _mm512_mask_blend_epi64(below, lane_base, base);
            lane_min = _mm512_mask_blend_epi64(below, lane_min, v);
        }
        uint64_t mins[8];
        uint64_t seconds[8];
        uint64_t bases[8];
        _mm512_storeu_si512(mins, lane_min);
        _mm512_storeu_si512(seconds, lane_second);
        _mm512_storeu_si512(bases, lane_base);
        for (size_t lane = 0; lane < 8; ++lane) {
            merge_min(mins[lane], seconds[lane], bases[lane] + lane, min, runner_up, index);
        }
#elif ALLIGATOR_SIMD_MODE == 3
        const __m256i sign = _mm256_set1_epi64x(static_cast<long long>(0x8000000000000000ull));
        __m256i lane_min = _mm256_set1_epi64x(-1);
        __m256i lane_second = _mm256_set1_epi64x(-1);
        __m256i lane_base = _mm256_setzero_si256();
        for (; i + 4 <= size; i += 4) {
            const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(words + i));
            const __m256i v_signed = _mm256_xor_si256(v, sign);
            const __m256i below = _mm256_cmpgt_epi64(_mm256_xor_si256(lane_min, sign), v_signed);
            const __m256i below_second = _mm256_cmpgt_epi64(
                _mm256_xor_si256(lane_second, sign), v_signed);
            const __m256i lowered = _mm256_blendv_epi8(lane_second, v, below_second);
            const __m256i base = _mm256_set1_epi64x(static_cast<long long>(i));
            lane_second = _mm256_blendv_epi8(lowered, lane_min, below);
            lane_base = _mm256_blendv_epi8(lane_base, base, below);
            lane_min = _mm256_blendv_epi8(lane_min, v, below);
        }
        uint64_t mins[4];
        uint64_t seconds[4];
        uint64_t bases[4];
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(mins), lane_min);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(seconds), lane_second);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(bases), lane_base);
        for (size_t lane = 0; lane < 4; ++lane) {
            merge_min(mins[lane], seconds[lane], bases[lane] + lane, min, runner_up, index);
        }
#elif ALLIGATOR_SIMD_MODE == 4
        const __m128i sign = _mm_set1_epi64x(static_cast<long long>(0x8000000000000000ull));
        __m128i lane_min = _mm_set1_epi64x(-1);
        __m128i lane_second = _mm_set1_epi64x(-1);
        __m128i lane_base = _mm_setzero_si128();
        for (; i + 2 <= size; i += 2) {
            const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(words + i));
            const __m128i v_signed = _mm_xor_si128(v, sign);
            const __m128i below = _mm_cmpgt_epi64(_mm_xor_si128(lane_min, sign), v_signed);
            const __m128i below_second = _mm_cmpgt_epi64(
                _mm_xor_si128(lane_second, sign), v_signed);
            const __m128i lowered = _mm_blendv_epi8(lane_second, v, below_second);
            const __m128i base = _mm_set1_epi64x(static_cast<long long>(i));
            lane_second = _mm_blendv_epi8(lowered, lane_min, below);
            lane_base = _mm_blendv_epi8(lane_base, base, below);
            lane_min = _mm_blendv_epi8(lane_min, v, below);
        }
        uint64_t mins[2];
        uint64_t seconds[2];
        uint64_t bases[2];
        _mm_storeu_si128(reinterpret_cast<__m128i*>(mins), lane_min);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(seconds), lane_second);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(bases), lane_base);
        for (size_t lane = 0; lane < 2; ++lane) {
            merge_min(mins[lane], seconds[lane], bases[lane] + lane, min, runner_up, index);
        }
#elif ALLIGATOR_SIMD_MODE == 5
        __vector unsigned long long lane_min = vec_splats(~0ull);
        __vector unsigned long long lane_second = vec_splats(~0ull);
        __vector unsigned long long lane_base = vec_splats(0ull);
        for (; i + 2 <= size; i += 2) {
            const __vector unsigned long long v =
                vec_xl(0, reinterpret_cast<const unsigned long long*>(words + i));
            const __vector bool long long below = vec_cmplt(v, lane_min);
            lane_second = vec_sel(vec_min(lane_second, v), lane_min, below);
            lane_base = vec_sel(lane_base, vec_splats(static_cast<unsigned long long>(i)), below);
            lane_min = vec_sel(lane_min, v, below);
        }
        for (size_t lane = 0; lane < 2; ++lane) {
            merge_min(lane_min[lane], lane_second[lane], lane_base[lane] + lane,
                min, runner_up, index);
        }
#elif ALLIGATOR_SIMD_MODE == 6
        uint64x2_t lane_min[2] = {vdupq_n_u64(~uint64_t(0)), vdupq_n_u64(~uint64_t(0))};
        uint64x2_t lane_second[2] = {vdupq_n_u64(~uint64_t(0)), vdupq_n_u64(~uint64_t(0))};
        uint64x2_t lane_base[2] = {vdupq_n_u64(0), vdupq_n_u64(0)};
        for (; i + 4 <= size; i += 4) {
            const uint64x2_t base = vdupq_n_u64(static_cast<uint64_t>(i));
            for (size_t half = 0; half < 2; ++half) {
                const uint64x2_t v = vld1q_u64(words + i + half * 2);
                const uint64x2_t below = vcltq_u64(v, lane_min[half]);
                const uint64x2_t below_second = vcltq_u64(v, lane_second[half]);
                const uint64x2_t lowered = vbslq_u64(below_second, v, lane_second[half]);
                lane_second[half] = vbslq_u64(below, lane_min[half], lowered);
                lane_base[half] = vbslq_u64(below, base, lane_base[half]);
                lane_min[half] = vbslq_u64(below, v, lane_min[half]);
            }
        }
        uint64_t mins[4];
        uint64_t seconds[4];
        uint64_t bases[4];
        for (size_t half = 0; half < 2; ++half) {
            vst1q_u64(mins + half * 2, lane_min[half]);
            vst1q_u64(seconds + half * 2, lane_second[half]);
            vst1q_u64(bases + half * 2, lane_base[half]);
        }
        for (size_t lane = 0; lane < 4; ++lane) {
            merge_min(mins[lane], seconds[lane], bases[lane] + lane, min, runner_up, index);
        }
#endif
        for (; i < size; ++i) merge_min(words[i], ~uint64_t(0), i, min, runner_up, index);
        return index;
    }
private:
#if ALLIGATOR_SIMD_MODE == 1
    /** ------------------------------------------------------------------------------------------- Reduce Max 8
     * @brief Largest of eight unsigned 64-bit lanes.
     * @param lanes The lanes.
     * @return The largest lane.
     */
    static uint64_t reduce_max8(simd_ulong8 lanes) {
        const simd_ulong4 quad = simd_max(lanes.lo, lanes.hi);
        const simd_ulong2 pair = simd_max(quad.lo, quad.hi);
        return pair.x > pair.y ? pair.x : pair.y;
    }
    /** ------------------------------------------------------------------------------------------- Reduce Min 8
     * @brief Smallest of eight unsigned 64-bit lanes.
     * @param lanes The lanes.
     * @return The smallest lane.
     */
    static uint64_t reduce_min8(simd_ulong8 lanes) {
        const simd_ulong4 quad = simd_min(lanes.lo, lanes.hi);
        const simd_ulong2 pair = simd_min(quad.lo, quad.hi);
        return pair.x < pair.y ? pair.x : pair.y;
    }
#endif
    /** ------------------------------------------------------------------------------------------- Merge Max
     * @brief Folds one lane's largest and second largest words into the running result.
     * @param lane_max The lane's largest word.
     * @param lane_second The lane's second largest word.
     * @param lane_index The slot holding the lane's largest word.
     * @param max The running largest word.
     * @param runner_up The running second largest word.
     * @param index The slot holding the running largest word.
     */
    static void merge_max(
        uint64_t lane_max,
        uint64_t lane_second,
        size_t lane_index,
        uint64_t& max,
        uint64_t& runner_up,
        size_t& index
    ) {
        const bool above = lane_max > max;
        runner_up = above ? max : (lane_max > runner_up ? lane_max : runner_up);
        max = above ? lane_max : max;
        index = above ? lane_index : index;
        runner_up = lane_second > runner_up ? lane_second : runner_up;
    }
    /** ------------------------------------------------------------------------------------------- Merge Min
     * @brief Folds one lane's smallest word into the running result.
     * @param lane_min The lane's smallest word.
     * @param lane_index The slot holding it.
     * @param min The running smallest word.
     * @param index The slot holding the running smallest word.
     */
    static void merge_min(uint64_t lane_min, size_t lane_index, uint64_t& min, size_t& index) {
        const bool below = lane_min < min;
        min = below ? lane_min : min;
        index = below ? lane_index : index;
    }
    /** ------------------------------------------------------------------------------------------- Merge Min With Runner-Up
     * @brief Folds one lane's smallest and second smallest words into the running result.
     * @param lane_min The lane's smallest word.
     * @param lane_second The lane's second smallest word.
     * @param lane_index The slot holding the lane's smallest word.
     * @param min The running smallest word.
     * @param runner_up The running second smallest word.
     * @param index The slot holding the running smallest word.
     */
    static void merge_min(
        uint64_t lane_min,
        uint64_t lane_second,
        size_t lane_index,
        uint64_t& min,
        uint64_t& runner_up,
        size_t& index
    ) {
        const bool below = lane_min < min;
        runner_up = below ? min : (lane_min < runner_up ? lane_min : runner_up);
        min = below ? lane_min : min;
        index = below ? lane_index : index;
        runner_up = lane_second < runner_up ? lane_second : runner_up;
    }
};
} // namespace buffetalligator
