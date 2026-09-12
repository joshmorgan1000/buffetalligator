/** --------------------------------------------------------------------------------------------------------- Claim Benchmark
 * @file bench_claim.cpp
 * @brief Measures claim latency, parallel throughput, and settled memory footprint.
 */
#include <alligator.hpp>
#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
/** --------------------------------------------------------------------------------------------------------- Samples
 * @brief Accumulates operation latency and threshold counts.
 */
struct Samples {
    uint64_t total = 0; ///< Total measured nanoseconds.
    uint64_t maximum = 0; ///< Largest measured latency.
    std::array<uint64_t, 4> over{}; ///< Counts exceeding each threshold.
    /** ------------------------------------------------------------------------------------------- Record
     * @brief Records one operation duration.
     */
    void record(uint64_t elapsed) {
        total += elapsed;
        maximum = std::max(maximum, elapsed);
        const std::array<uint64_t, 4> thresholds{1000, 10000, 100000, 1000000};
        for (size_t index = 0; index < thresholds.size(); ++index) {
            over[index] += elapsed > thresholds[index];
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Print
 * @brief Prints latency statistics for a measured row.
 */
void print(const char* label, const Samples& samples, size_t count, double wall) {
    std::printf("%s: mean %.2f ns, wall %.2f ns/op, max %.3f us, over 1us/10us/100us/1ms %llu/%llu/%llu/%llu\n",
        label, double(samples.total) / count, wall, double(samples.maximum) / 1000,
        (unsigned long long)samples.over[0], (unsigned long long)samples.over[1],
        (unsigned long long)samples.over[2], (unsigned long long)samples.over[3]);
}
/** --------------------------------------------------------------------------------------------------------- Claim Worker
 * @brief Measures one million claim and release operations after a synchronized start.
 */
void claims(Samples* samples, std::barrier<>* start) {
    start->arrive_and_wait();
    for (size_t operation = 0; operation < 1000000; ++operation) {
        const auto begin = Clock::now();
        { buffetalligator::Slice slice(1024); }
        samples->record(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
    }
}
/** --------------------------------------------------------------------------------------------------------- Parallel
 * @brief Measures synchronized claim workers and aggregate wall time.
 */
void parallel(size_t count) {
    std::vector<Samples> samples(count);
    std::vector<std::thread> threads;
    std::barrier start(static_cast<std::ptrdiff_t>(count + 1));
    for (size_t index = 0; index < count; ++index) {
        threads.emplace_back(claims, &samples[index], &start);
    }
    const auto begin = Clock::now();
    start.arrive_and_wait();
    for (auto& thread : threads) thread.join();
    const double wall = std::chrono::duration<double, std::nano>(Clock::now() - begin).count();
    Samples aggregate;
    for (size_t index = 0; index < count; ++index) {
        std::printf("%zu threads, worker %zu mean %.2f ns\n", count, index, double(samples[index].total) / 1000000);
        aggregate.total += samples[index].total;
        aggregate.maximum = std::max(aggregate.maximum, samples[index].maximum);
        for (size_t threshold = 0; threshold < 4; ++threshold) aggregate.over[threshold] += samples[index].over[threshold];
    }
    const std::string label = std::to_string(count) + " threads claim+free";
    print(label.c_str(), aggregate, count * 1000000, wall / (count * 1000000));
}
/** --------------------------------------------------------------------------------------------------------- Footprint
 * @brief Reads the current process resident footprint from the operating system.
 */
uint64_t footprint() {
#if defined(__APPLE__)
    task_vm_info_data_t information{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&information), &count) != KERN_SUCCESS) {
        throw std::runtime_error("task_info failed");
    }
    return information.phys_footprint;
#elif defined(__linux__)
    FILE* file = std::fopen("/proc/self/statm", "r");
    unsigned long long virtual_pages, resident_pages;
    if (!file) throw std::runtime_error("statm open failed");
    const int fields = std::fscanf(file, "%llu %llu", &virtual_pages, &resident_pages);
    std::fclose(file);
    if (fields != 2) throw std::runtime_error("statm read failed");
    return resident_pages * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
#else
#error Benchmark footprint requires a supported operating system
#endif
}
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs the benchmark rows and reports memory after a three-second settle.
 */
int main() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    for (size_t threads : {1, 4, 8, 16}) parallel(threads);
    {
        buffetalligator::Slice parent(1024);
        Samples samples;
        for (size_t operation = 0; operation < 1000000; ++operation) {
            const auto begin = Clock::now();
            { auto view = parent.slice(64, 128); }
            samples.record(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
        }
        print("view+drop", samples, 1000000, double(samples.total) / 1000000);
    }
    Samples novel;
    for (size_t operation = 0; operation < 2000; ++operation) {
        const auto begin = Clock::now();
        { buffetalligator::Slice slice(1024 * 1024, true); }
        novel.record(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
    }
    print("1 MiB novel", novel, 2000, double(novel.total) / 2000);
    std::this_thread::sleep_for(std::chrono::seconds(3));
    std::printf("after 3 s: allocated %zu, freed %zu, footprint %llu bytes\n",
        buffetalligator::Memory::total_allocations(), buffetalligator::Memory::total_freed(),
        (unsigned long long)footprint());
    for (uint16_t type = 0; type < 2; ++type) {
        const auto& placement = *buffetalligator::BuffetMenu::get(type);
        std::printf("placement %u: usage %zu, reserved %zu, runway target %zu, slab %zu bytes\n",
            type, buffetalligator::Memory::placement_usage(placement),
            buffetalligator::Memory::placement_reserved(placement),
            buffetalligator::Memory::placement_runway_target(placement),
            buffetalligator::Memory::placement_slab_size(placement));
    }
}
