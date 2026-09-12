/** --------------------------------------------------------------------------------------------------------- Header Fixes Test
 * @file header_fixes_test.cpp
 * @brief Verifies bounded weak copies, typed views, and runtime-sized hazard registration.
 */
#include "../test_support.hpp"
#include <alligator.hpp>
#include <array>
#include <atomic>
#include <barrier>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
/** --------------------------------------------------------------------------------------------------------- Concurrent Map
 * @brief Publishes eight rows while all registered threads concurrently search the map.
 */
void concurrent_map(buffetalligator::SliceMap* map, unsigned thread, std::barrier<>* start) {
    start->arrive_and_wait();
    if (thread < 8) {
        buffetalligator::Slice payload(64);
        payload.get_as<uint64_t>() = thread + 17;
        map->add_slice(static_cast<int64_t>(thread), std::move(payload));
    }
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
        for (int64_t identifier = 0; identifier < 8; ++identifier) {
            auto payload = map->get_slice(identifier);
            if (payload) TEST_EQUAL(payload.get_as<uint64_t>(), static_cast<uint64_t>(identifier + 17), "wrong concurrent payload");
        }
    }
}
/** --------------------------------------------------------------------------------------------------------- Reset Reader
 * @brief Copies protected payloads while the producer resets and republishes rows.
 */
void reset_reader(buffetalligator::SliceMap* map, std::atomic<bool>* running) {
    while (running->load(std::memory_order_acquire)) {
        for (int64_t identifier = 0; identifier < 8; ++identifier) {
            auto payload = map->get_slice(identifier);
            if (payload) TEST_EQUAL(payload.get_as<uint64_t>(), static_cast<uint64_t>(identifier + 17), "reset reclaimed a live reader");
        }
    }
}
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Exercises each header regression and hazard-row reuse across thread waves.
 */
int main() {
    test_support::start(__FILE__);
    using namespace buffetalligator;
    std::array<uint8_t, 256> source;
    for (size_t index = 0; index < source.size(); ++index) source[index] = static_cast<uint8_t>(index);
    Slice sibling_before(256);
    std::memset(sibling_before.raw(), 0xe4, sibling_before.size_bytes());
    WeakSlice weak(source.data(), source.size());
    Slice copy = weak.slice(64, 128);
    Slice sibling_after(256);
    TEST_REQUIRE(copy.size_bytes() == 128 && std::memcmp(copy.raw(), source.data() + 64, 128) == 0, "weak copy offset");
    for (size_t index = 0; index < 256; ++index) {
        TEST_EQUAL(sibling_before.data<uint8_t>()[index], 0xe4, "weak copy damaged previous sibling");
        TEST_EQUAL(sibling_after.data<uint8_t>()[index], 0, "weak copy overran destination");
    }
    SliceT<int> numbers(Slice(16));
    TEST_EQUAL(numbers.length(), 4, "primitive typed length");
    SliceT<std::string_view> text(Slice(32));
    TEST_EQUAL(text.view().size(), 32, "string view length");
    SliceT<std::span<int>> span(Slice(32));
    TEST_EQUAL(span.view().size(), 8, "span view length");
    {
        SliceMap map(8);
        std::atomic<bool> running{true};
        std::thread reader(reset_reader, &map, &running);
        for (unsigned wave = 0; wave < 1000; ++wave) {
            map.reset();
            for (int64_t identifier = 0; identifier < 8; ++identifier) {
                Slice payload(64); payload.get_as<uint64_t>() = identifier + 17;
                map.add_slice(identifier, std::move(payload));
            }
        }
        running.store(false, std::memory_order_release);
        reader.join();
    }
    const unsigned threads = 2 * Memory::hardware_threads();
    for (unsigned wave = 0; wave < 3; ++wave) {
        SliceMap map(8);
        if (threads < 8) {
            for (unsigned identifier = threads; identifier < 8; ++identifier) {
                Slice payload(64); payload.get_as<uint64_t>() = identifier + 17;
                map.add_slice(static_cast<int64_t>(identifier), std::move(payload));
            }
        }
        std::barrier start(static_cast<std::ptrdiff_t>(threads));
        std::vector<std::thread> workers;
        for (unsigned thread = 0; thread < threads; ++thread) workers.emplace_back(concurrent_map, &map, thread, &start);
        for (auto& worker : workers) worker.join();
        TEST_EQUAL(map.size(), 8, "missing published rows");
        for (int64_t identifier = 0; identifier < 8; ++identifier) TEST_REQUIRE(map.find(identifier) >= 0, "missing map ID");
    }
}
