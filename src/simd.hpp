#pragma once
/** --------------------------------------------------------------------------------------------------------- SIMD
 * @file simd.hpp
 * @brief SIMD utility functions and classes for the Buffet Alligator project.
 */
#ifdef ALLIGATOR_SIMD_MODE
#undef ALLIGATOR_SIMD_MODE
#endif
#if (__has_include(<simd/simd.h>))
    #include <simd/simd.h>
    #define ALLIGATOR_SIMD_MODE 1
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
            const simd_long8 id_v = simd_make_long8(id);
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
            const simd_ulong8 id_v = simd_make_ulong8(static_cast<uint64_t>(id));
            #pragma unroll 8
            for (; i + 8 <= size; i += 8) {
                const simd_ulong8 ids_v = *reinterpret_cast<const simd_packed_ulong8*>(ids + i);
                if (simd_any(ids_v == id_v)) {
                    #pragma unroll
                    for (int j = 0; j < 8; ++j) {
                        if (ids[i + j] == id) return static_cast<int64_t>(i + j);
                    }
                }
            }
        } else if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, int>) {
            const simd_int16 id_v = simd_make_int16(static_cast<int32_t>(id));
            #pragma unroll 8
            for (; i + 16 <= size; i += 16) {
                const simd_int16 ids_v = *reinterpret_cast<const simd_packed_int16*>(ids + i);
                if (simd_any(ids_v == id_v)) {
                    #pragma unroll
                    for (int j = 0; j < 16; ++j) {
                        if (ids[i + j] == id) return static_cast<int64_t>(i + j);
                    }
                }
            }
        } else if constexpr (std::is_same_v<T, uint32_t> || std::is_same_v<T, unsigned int>) {
            const simd_uint16 id_v = simd_make_uint16(static_cast<uint32_t>(id));
            #pragma unroll 8
            for (; i + 16 <= size; i += 16) {
                const simd_uint16 ids_v = *reinterpret_cast<const simd_packed_uint16*>(ids + i);
                if (simd_any(ids_v == id_v)) {
                    #pragma unroll
                    for (int j = 0; j < 16; ++j) {
                        if (ids[i + j] == id) return static_cast<int64_t>(i + j);
                    }
                }
            }
        } else if constexpr (std::is_same_v<T, int16_t> || std::is_same_v<T, short>) {
            const simd_short32 id_v = simd_make_short32(static_cast<int16_t>(id));
            const simd_short8 id_v8 = simd_make_short8(static_cast<int16_t>(id));
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
                                if (ids[i + j * 8 + k] == id) return static_cast<int64_t>(i + j * 8 + k);
                            }
                        }
                    }
                }
            }
        } else if constexpr (std::is_same_v<T, uint16_t> || std::is_same_v<T, unsigned short>) {
            const simd_ushort32 id_v = simd_make_ushort32(static_cast<uint16_t>(id));
            const simd_ushort8 id_v8 = simd_make_ushort8(static_cast<uint16_t>(id));
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
                                if (ids[i + j * 8 + k] == id) return static_cast<int64_t>(i + j * 8 + k);
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
                if (mask != 0) return static_cast<int64_t>(i + __builtin_ctz(mask));
            }
        } else if constexpr (sizeof(T) == 4) {
            const __m512i id_v = _mm512_set1_epi32(static_cast<int32_t>(id));
            for (; i + 16 <= size; i += 16) {
                const __m512i ids_v = _mm512_loadu_si512(ids + i);
                const __mmask16 mask = _mm512_cmpeq_epi32_mask(ids_v, id_v);
                if (mask != 0) return static_cast<int64_t>(i + __builtin_ctz(mask));
            }
        } else if constexpr (sizeof(T) == 2) {
            const __m512i id_v = _mm512_set1_epi16(static_cast<int16_t>(id));
            for (; i + 32 <= size; i += 32) {
                const __m512i ids_v = _mm512_loadu_si512(ids + i);
                const __mmask32 mask = _mm512_cmpeq_epi16_mask(ids_v, id_v);
                if (mask != 0) return static_cast<int64_t>(i + __builtin_ctz(mask));
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
                if (mask != 0) return static_cast<int64_t>(i + (__builtin_ctz(mask) >> 3));
            }
        } else if constexpr (sizeof(T) == 4) {
            const __m256i id_v = _mm256_set1_epi32(static_cast<int32_t>(id));
            for (; i + 8 <= size; i += 8) {
                const __m256i ids_v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ids + i));
                const int mask = _mm256_movemask_epi8(_mm256_cmpeq_epi32(ids_v, id_v));
                if (mask != 0) return static_cast<int64_t>(i + (__builtin_ctz(mask) >> 2));
            }
        } else if constexpr (sizeof(T) == 2) {
            const __m256i id_v = _mm256_set1_epi16(static_cast<int16_t>(id));
            for (; i + 16 <= size; i += 16) {
                const __m256i ids_v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ids + i));
                const int mask = _mm256_movemask_epi8(_mm256_cmpeq_epi16(ids_v, id_v));
                if (mask != 0) return static_cast<int64_t>(i + (__builtin_ctz(mask) >> 1));
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
                if (mask != 0) return static_cast<int64_t>(i + (__builtin_ctz(mask) >> 3));
            }
        } else if constexpr (sizeof(T) == 4) {
            const __m128i id_v = _mm_set1_epi32(static_cast<int32_t>(id));
            for (; i + 4 <= size; i += 4) {
                const __m128i ids_v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ids + i));
                const int mask = _mm_movemask_epi8(_mm_cmpeq_epi32(ids_v, id_v));
                if (mask != 0) return static_cast<int64_t>(i + (__builtin_ctz(mask) >> 2));
            }
        } else if constexpr (sizeof(T) == 2) {
            const __m128i id_v = _mm_set1_epi16(static_cast<int16_t>(id));
            for (; i + 8 <= size; i += 8) {
                const __m128i ids_v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ids + i));
                const int mask = _mm_movemask_epi8(_mm_cmpeq_epi16(ids_v, id_v));
                if (mask != 0) return static_cast<int64_t>(i + (__builtin_ctz(mask) >> 1));
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
                    if (ids[i] == id) return static_cast<int64_t>(i);
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
                        if (ids[i + j] == id) return static_cast<int64_t>(i + j);
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
                        if (ids[i + j] == id) return static_cast<int64_t>(i + j);
                    }
                }
            }
        } else {
            static_assert(always_false<T>::value, "Unsupported type for SIMD search");
        }
#endif
        // Check remaining elements
        for (; i < size; ++i) {
            if (ids[i] == id) {
                return static_cast<int64_t>(i);
            }
        }
        return -1;
    }
};
} // namespace buffetalligator
