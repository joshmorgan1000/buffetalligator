/** --------------------------------------------------------------------------------------------------------- OS Layer Test
 * @file os_layer_test.cpp
 * @brief Verifies page mapping, system probes, wake events, and thread-exit cleanup.
 */
extern "C" {
#include "core/ba_os.h"
}
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace {
std::atomic<bool> ran{false};
std::atomic<bool> destroyed{false};
/** --------------------------------------------------------------------------------------------------------- Require
 * @brief Fails an operating-system contract assertion.
 */
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
/** --------------------------------------------------------------------------------------------------------- Thread Exit
 * @brief Records completion of native TLS cleanup.
 */
void thread_exit(void* argument) {
    static_cast<std::atomic<bool>*>(argument)->store(true, std::memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Thread Entry
 * @brief Registers cleanup and signals execution to the test thread.
 */
void thread_entry(void*) {
    ba_os_tls_set(&destroyed);
    ran.store(true, std::memory_order_release);
}
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Exercises all platform services through their C interface.
 */
int main() {
    ba_sysinfo_t information;
    ba_os_probe(&information);
    require(information.page >= 4096 && !(information.page & (information.page - 1)), "invalid page size");
    require(information.granule % information.page == 0, "invalid granule");
    require(information.hw_threads >= 1, "invalid processor count");
    require(information.physical > 0 && information.available <= information.physical, "invalid capacity");
    std::printf("effective process limit: %llu\n", (unsigned long long)information.limit);
    const size_t bytes = 4 * information.granule;
    auto* memory = static_cast<unsigned char*>(ba_os_map(bytes, information.granule, 0));
    require(memory && reinterpret_cast<uintptr_t>(memory) % information.granule == 0, "unaligned mapping");
    require(memory[0] == 0 && memory[bytes - 1] == 0, "mapping not zeroed");
    std::memset(memory, 0xa7, bytes);
    require(ba_os_rezero(memory, bytes) == 0, "remapping failed");
    for (size_t index = 0; index < bytes; ++index) require(memory[index] == 0, "remapping retained data");
    ba_os_unmap(memory, bytes);
    const uint64_t before = ba_os_now_ns();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(ba_os_now_ns() > before, "clock did not advance");
    ba_event_t* event = ba_event_create();
    require(event != nullptr, "event allocation failed");
    ba_event_signal(event);
    const uint64_t signal_start = ba_os_now_ns();
    ba_event_wait(event, 1000000000);
    require(ba_os_now_ns() - signal_start < 500000000, "signal before wait was lost");
    ba_event_destroy(event);
    require(ba_os_tls_key(thread_exit) == 0, "TLS key creation failed");
    require(ba_os_thread_start(thread_entry, nullptr) == 0, "thread creation failed");
    const uint64_t deadline = ba_os_now_ns() + 1000000000;
    while (!destroyed.load(std::memory_order_acquire) && ba_os_now_ns() < deadline) ba_os_yield();
    require(ran.load(std::memory_order_acquire), "thread did not run");
    require(destroyed.load(std::memory_order_acquire), "TLS cleanup did not run");
    require(ba_os_pressure() <= BA_PRESSURE_CRITICAL, "invalid pressure");
}
