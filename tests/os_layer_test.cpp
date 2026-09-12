/** --------------------------------------------------------------------------------------------------------- OS Layer Test
 * @file os_layer_test.cpp
 * @brief Verifies page mapping, system probes, wake events, and thread-exit cleanup.
 */
#include "test_support.hpp"
extern "C" {
#include "core/ba_os.h"
}
#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace {
std::atomic<bool> ran{false};
std::atomic<bool> destroyed{false};
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
    test_support::start(__FILE__);
    ba_sysinfo_t information;
    ba_os_probe(&information);
    TEST_REQUIRE(information.page >= 4096 && !(information.page & (information.page - 1)), "invalid page size");
    TEST_EQUAL(information.granule % information.page, 0, "invalid granule");
    TEST_REQUIRE(information.hw_threads >= 1, "invalid processor count");
    TEST_REQUIRE(information.physical > 0 && information.available <= information.physical, "invalid capacity");
    std::printf("effective process limit: %llu\n", (unsigned long long)information.limit);
    const size_t bytes = 4 * information.granule;
    auto* memory = static_cast<unsigned char*>(ba_os_map(bytes, information.granule, 0));
    TEST_REQUIRE(memory && reinterpret_cast<uintptr_t>(memory) % information.granule == 0, "unaligned mapping");
    TEST_REQUIRE(memory[0] == 0 && memory[bytes - 1] == 0, "mapping not zeroed");
    std::memset(memory, 0xa7, bytes);
    TEST_EQUAL(ba_os_rezero(memory, bytes), 0, "remapping failed");
    for (size_t index = 0; index < bytes; ++index) TEST_EQUAL(memory[index], 0, "remapping retained data");
    ba_os_unmap(memory, bytes);
    auto* reserved = static_cast<unsigned char*>(ba_os_reserve(bytes));
    TEST_REQUIRE(reserved != nullptr, "virtual reservation failed");
    TEST_EQUAL(ba_os_commit(reserved, information.page), 0, "initial commit failed");
    TEST_REQUIRE(reserved[0] == 0 && reserved[information.page - 1] == 0, "commit was not zeroed");
    reserved[0] = 0x5a;
    reserved[information.page - 1] = 0x6b;
    TEST_EQUAL(ba_os_commit(reserved + information.page, bytes - information.page), 0,
        "reservation extension failed");
    TEST_REQUIRE(reserved[0] == 0x5a && reserved[information.page - 1] == 0x6b,
        "extension changed existing pages");
    TEST_REQUIRE(reserved[information.page] == 0 && reserved[bytes - 1] == 0,
        "extension was not zeroed");
    ba_os_unmap(reserved, bytes);
    ba_mutex_t* mutex = ba_mutex_create();
    TEST_REQUIRE(mutex != nullptr, "mutex allocation failed");
    size_t protected_count = 0;
    std::array<std::thread, 4> contenders;
    for (auto& contender : contenders) {
        contender = std::thread([&] {
            for (size_t index = 0; index < 10000; ++index) {
                ba_mutex_lock(mutex);
                ++protected_count;
                ba_mutex_unlock(mutex);
            }
        });
    }
    for (auto& contender : contenders) contender.join();
    TEST_EQUAL(protected_count, 40000, "mutex lost a protected update");
    ba_mutex_destroy(mutex);
    const uint64_t before = ba_os_now_ns();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    TEST_REQUIRE(ba_os_now_ns() > before, "clock did not advance");
    ba_event_t* event = ba_event_create();
    TEST_REQUIRE(event != nullptr, "event allocation failed");
    ba_event_signal(event);
    const uint64_t signal_start = ba_os_now_ns();
    ba_event_wait(event, 1000000000);
    TEST_REQUIRE(ba_os_now_ns() - signal_start < 500000000, "signal before wait was lost");
    ba_event_destroy(event);
    TEST_EQUAL(ba_os_tls_key(thread_exit), 0, "TLS key creation failed");
    TEST_EQUAL(ba_os_thread_start(thread_entry, nullptr), 0, "thread creation failed");
    const uint64_t deadline = ba_os_now_ns() + 1000000000;
    while (!destroyed.load(std::memory_order_acquire) && ba_os_now_ns() < deadline) ba_os_yield();
    TEST_REQUIRE(ran.load(std::memory_order_acquire), "thread did not run");
    TEST_REQUIRE(destroyed.load(std::memory_order_acquire), "TLS cleanup did not run");
    TEST_REQUIRE(ba_os_pressure() <= BA_PRESSURE_CRITICAL, "invalid pressure");
}
