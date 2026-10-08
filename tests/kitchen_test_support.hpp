#pragma once
/** --------------------------------------------------------------------------------------------------------- Kitchen Test Support
 * @file kitchen_test_support.hpp
 * @brief Supplies named latch callbacks for asynchronous integration tests.
 */
#include <atomic>
#include <latch>
#include <memory>
#include <thread>

namespace kitchen_test {
inline std::atomic<size_t> active_callbacks{0};
/** --------------------------------------------------------------------------------------------------------- Latch Arrive
 * @brief Completes one asynchronous callback observed by the test latch.
 * @param context The caller-owned latch.
 */
inline void latch_arrive(void* context) {
    active_callbacks.fetch_add(1, std::memory_order_acq_rel);
    static_cast<std::latch*>(context)->count_down();
    active_callbacks.fetch_sub(1, std::memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Rearm Latch
 * @brief Reconstructs a test latch after named latch callbacks have returned.
 * @param completed The quiescent completion latch.
 * @param count The next callback count.
 */
inline void rearm_latch(std::latch& completed, std::ptrdiff_t count) {
    completed.wait();
    while (active_callbacks.load(std::memory_order_acquire) != 0) std::this_thread::yield();
    std::destroy_at(&completed);
    std::construct_at(&completed, count);
}
}
