/** --------------------------------------------------------------------------------------------------------- SliceMap Benchmark
 * @file bench_slicemap.cpp
 * @brief Separates append, successful lookup, and missing lookup costs with persistent workers.
 */
#include <alligator.hpp>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {
using buffetalligator::Slice;
using buffetalligator::SliceMap;
using Clock = std::chrono::steady_clock;
/** --------------------------------------------------------------------------------------------------------- Identifier
 * @brief Produces unique nonsequential positive IDs without the reserved sentinel.
 */
int64_t identifier(size_t index) {
    return static_cast<int64_t>((uint64_t(index + 1) * UINT64_C(0x5851f42d4c957f2d)) >> 1);
}
/** --------------------------------------------------------------------------------------------------------- Run
 * @brief Excludes allocation, reset, and worker startup from repeated map operations.
 */
void run(size_t threads, size_t capacity, size_t rounds, const char* operation) {
    SliceMap map(capacity);
    const bool append = std::string_view(operation) == "append";
    const bool copy = std::string_view(operation) == "get_hit";
    const bool missing = std::string_view(operation) == "find_miss";
    if (!append) {
        for (size_t index = 0; index < capacity; ++index) {
            Slice payload(64);
            payload.get_as<uint64_t>() = index;
            map.add_slice(identifier(index), std::move(payload));
        }
    }
    std::barrier phase(static_cast<std::ptrdiff_t>(threads + 1));
    std::vector<std::thread> workers;
    std::vector<uint64_t> checksums(threads);
    constexpr size_t lookups = 2048;
    for (size_t worker = 0; worker < threads; ++worker) {
        workers.emplace_back([&, worker] {
            const size_t first = capacity * worker / threads;
            const size_t last = capacity * (worker + 1) / threads;
            Slice backing(append ? (last - first) * 64 : 0, true);
            std::vector<Slice> staged(append ? last - first : 0);
            uint64_t checksum = 0;
            for (size_t round = 0; round <= rounds; ++round) {
                for (size_t index = 0; index < staged.size(); ++index) {
                    staged[index] = backing.slice(index * 64, 64);
                }
                phase.arrive_and_wait();
                phase.arrive_and_wait();
                if (append) {
                    for (size_t index = first; index < last; ++index) {
                        map.add_slice(identifier(index), std::move(staged[index - first]));
                    }
                } else {
                    for (size_t index = 0; index < lookups; ++index) {
                        const size_t target = (index * 8191 + worker * 127 + round * 31) % capacity;
                        const int64_t needle = missing ? -2 : identifier(target);
                        if (copy) {
                            auto payload = map.get_slice(needle);
                            if (!payload || payload.get_as<uint64_t>() != target) std::abort();
                            checksum += payload.get_as<uint64_t>();
                        } else {
                            const int64_t found = map.find(needle);
                            if (found != (missing ? -1 : static_cast<int64_t>(target))) std::abort();
                            checksum += static_cast<uint64_t>(found);
                        }
                    }
                }
                phase.arrive_and_wait();
            }
            checksums[worker] = checksum;
        });
    }
    double seconds = 0;
    for (size_t round = 0; round <= rounds; ++round) {
        if (append) map.reset();
        phase.arrive_and_wait();
        const auto start = Clock::now();
        phase.arrive_and_wait();
        phase.arrive_and_wait();
        const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        if (round) seconds += elapsed;
        if (append && map.size() != capacity) std::abort();
    }
    for (auto& worker : workers) worker.join();
    uint64_t checksum = 0;
    for (uint64_t value : checksums) checksum += value;
    const size_t operations = rounds * (append ? capacity : lookups * threads);
    std::printf("%s,%zu,%zu,%zu,%.9f,%.6f,%llu\n", operation, threads, capacity,
        operations, seconds, operations / seconds / 1e6, static_cast<unsigned long long>(checksum));
    std::fflush(stdout);
}
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs the selected worker count and capacity with one untimed warmup round.
 */
int main(int argc, char** argv) {
    const size_t threads = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 8;
    const size_t capacity = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 65536;
    const size_t rounds = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 8;
    if (!threads || capacity < threads || !rounds) return 2;
    std::printf("operation,threads,capacity,operations,seconds,million_ops_s,checksum\n");
    for (const char* operation : {"append", "find_hit", "find_miss", "get_hit"}) {
        run(threads, capacity, rounds, operation);
    }
}
