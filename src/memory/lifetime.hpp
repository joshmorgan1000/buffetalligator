#pragma once
/** --------------------------------------------------------------------------------------------------------- Runtime Lifetime
 * @file lifetime.hpp
 * @brief Retains lazily constructed runtime owners until public-header static owners have retired.
 */
#include <cstddef>
#include <type_traits>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Runtime Finalizer
 * @brief Links one fully constructed owner into allocation-free reverse-order finalization.
 */
struct RuntimeFinalizer {
    enum class Phase { TrackerDetach, Registry, Arena, Runtime };
    void* object;
    void (*destroy)(void*);
    RuntimeFinalizer* previous;
    Phase phase;
    RuntimeFinalizer(void* object, void (*destroy)(void*), Phase phase = Phase::Runtime);
    /** ------------------------------------------------------------------------------------------- Delete
     * @brief Destroys the concrete owner after its dependent runtime objects have retired.
     */
    template <typename Owner> static void delete_owner(void* object) {
        delete static_cast<Owner*>(object);
    }
};
static_assert(std::is_trivially_destructible_v<RuntimeFinalizer>);
} // namespace buffetalligator
