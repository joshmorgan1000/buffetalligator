#pragma once
/** --------------------------------------------------------------------------------------------------------- Generational Reference Counter
 * @file genrefcounter.hpp
 * @brief Related structures and classes for the generational reference counter.
 */
#include <alligator.hpp>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- OnZero Callback Structure
 * @struct OnZero
 * @brief Structure representing a callback to be invoked when a reference count reaches zero.
 */
struct OnZero {
    static std::unordered_map<void(*)(void*), uint32_t>& registry() {
        static std::unordered_map<void(*)(void*), uint32_t> reg;
        return reg;
    }
    static std::shared_mutex& registry_mutex() {
        static std::shared_mutex mtx;
        return mtx;
    }
    void (*callback)(void*);
};
/** --------------------------------------------------------------------------------------------------------- Entry Structure
 * @struct Entry
 * @brief Structure representing an entry in the generational reference counter.
 */
struct Entry {
    static std::array<Entry*, 2>& bases() {
        static std::array<Entry*, 2> base_ptr = {nullptr, nullptr};
        return base_ptr;
    }
    static std::array<OnZero*, 2>& on_zero_bases() {
        static std::array<OnZero*, 2> base_ptr = {nullptr, nullptr};
        return base_ptr;
    }
    void* context;
    uint32_t mycount;
    uint32_t parentidx = 0;
    void clear() { mycount = 0; parentidx = 0; context = nullptr; }
    void inc() { mycount += 1 << 14; }
    void dec() {
        if ((mycount -= 1 << 14) >> 14 == 0) {
            if ((parentidx & 2) >> 1) {
                const size_t pidx = parentidx >> 3;
                Entry* parent = Entry::bases()[(parentidx & 2) >> 1] + pidx;
                parent->dec();
            } else {
                OnZero* oz = on_zero();
                if (oz && oz->callback) oz->callback(context);
            }
            clear();
        }
    }
    OnZero* on_zero() {
        return const_cast<OnZero*>(on_zero_bases()[(parentidx & 2) >> 1] + (mycount & 0x3FFF));
    }
};
} // namespace buffetalligator