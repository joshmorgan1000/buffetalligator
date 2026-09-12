/** --------------------------------------------------------------------------------------------------------- Arena Performance
 * @file performance.cpp
 * @brief Measures the real library's claim, reference, allocation, and queue workloads.
 */
#include <alligator.hpp>
extern "C" {
#include "core/ba_core.h"
}
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <semaphore>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace buffetalligator;
using Clock = std::chrono::steady_clock;
std::atomic<uint64_t> observed{0};
bool failed_operations = false;
enum class Operation { claim, copy, view, aligned, novel };
/** --------------------------------------------------------------------------------------------------------- Require
 * @brief Stops a benchmark row when its workload fails correctness checks.
 */
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
/** --------------------------------------------------------------------------------------------------------- Nanoseconds
 * @brief Returns elapsed monotonic nanoseconds for one timed interval.
 */
uint64_t nanoseconds(Clock::time_point begin) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
}
/** --------------------------------------------------------------------------------------------------------- Statistics
 * @brief Reads the actual allocator's cumulative byte counters.
 */
ba_stats_t statistics() {
    ba_stats_t result;
    ba_stats_total(&result);
    return result;
}
/** --------------------------------------------------------------------------------------------------------- Output
 * @brief Emits throughput, sampled latency, and allocator activity for one repetition.
 */
void output(const char* scenario, unsigned repetition, size_t threads, size_t bytes, size_t operations,
    uint64_t elapsed, std::vector<uint64_t> samples, const ba_stats_t& before, const ba_stats_t& after,
    size_t failures = 0) {
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&](size_t numerator) -> uint64_t {
        return samples.empty() ? 0 : samples[(samples.size() - 1) * numerator / 100];
    };
    std::printf("%s,%u,%zu,%zu,%zu,%llu,%.3f,%.3f,%llu,%llu,%llu,%llu,%llu,%llu,%zu,%zu\n",
        scenario, repetition, threads, bytes, operations, (unsigned long long)elapsed,
        operations ? static_cast<double>(elapsed) / operations : 0, static_cast<double>(operations) * 1000.0 / elapsed,
        (unsigned long long)percentile(50), (unsigned long long)percentile(95),
        (unsigned long long)percentile(99), (unsigned long long)(samples.empty() ? 0 : samples.back()),
        (unsigned long long)(after.slab_bytes_allocated - before.slab_bytes_allocated),
        (unsigned long long)(after.novel_bytes_allocated - before.novel_bytes_allocated), failures, samples.size());
}
/** --------------------------------------------------------------------------------------------------------- Operation
 * @brief Runs one actual library operation and returns observable payload data.
 */
template<Operation Kind>
uint64_t operation(size_t bytes, const Slice& parent) {
    if constexpr (Kind == Operation::copy) {
        Slice copy = parent;
        return copy.data()[0];
    } else if constexpr (Kind == Operation::view) {
        Slice view = parent.slice(0, bytes);
        return view.data()[0];
    } else if constexpr (Kind == Operation::aligned) {
        void* pointer = nullptr;
        if (posix_memalign(&pointer, 64, bytes)) std::abort();
        std::memset(pointer, 0, bytes);
        asm volatile("" : : "r"(pointer) : "memory");
        auto* payload = static_cast<volatile unsigned char*>(pointer);
        payload[0] = 17;
        payload[bytes - 1] = 31;
        const uint64_t value = payload[0] + payload[bytes - 1];
        std::free(pointer);
        return value;
    } else {
        Slice slice(bytes, Kind == Operation::novel);
        auto* payload = static_cast<volatile unsigned char*>(slice.raw());
        payload[0] = 17;
        payload[bytes - 1] = 31;
        return payload[0] + payload[bytes - 1];
    }
}
/** --------------------------------------------------------------------------------------------------------- Throughput
 * @brief Times synchronized persistent workers after constructing threads and warming their claims.
 */
template<Operation Kind>
void throughput(const char* scenario, unsigned repetition, size_t threads, size_t bytes, size_t count) {
    Slice parent(bytes);
    parent.data()[0] = 7;
    std::vector<std::thread> workers;
    std::barrier prepared(static_cast<std::ptrdiff_t>(threads + 1));
    std::barrier started(static_cast<std::ptrdiff_t>(threads + 1));
    std::barrier completed(static_cast<std::ptrdiff_t>(threads + 1));
    for (size_t worker = 0; worker < threads; ++worker) {
        workers.emplace_back([&] {
            uint64_t checksum = 0;
            for (size_t index = 0; index < 1024; ++index) checksum += operation<Kind>(bytes, parent);
            prepared.arrive_and_wait();
            started.arrive_and_wait();
            for (size_t index = 0; index < count; ++index) checksum += operation<Kind>(bytes, parent);
            observed.fetch_add(checksum, std::memory_order_relaxed);
            completed.arrive_and_wait();
        });
    }
    prepared.arrive_and_wait();
    const auto before = statistics();
    const auto begin = Clock::now();
    started.arrive_and_wait();
    completed.arrive_and_wait();
    const uint64_t elapsed = nanoseconds(begin);
    const auto after = statistics();
    for (auto& worker : workers) worker.join();
    output(scenario, repetition, threads, bytes, threads * count, elapsed, {}, before, after);
}
/** --------------------------------------------------------------------------------------------------------- Latency
 * @brief Samples each claim in a separate single-threaded latency run.
 */
template<Operation Kind>
void latency(const char* scenario, unsigned repetition, size_t bytes, size_t count) {
    Slice parent(bytes);
    std::vector<uint64_t> samples(count);
    for (size_t index = 0; index < 1024; ++index) operation<Kind>(bytes, parent);
    const auto before = statistics();
    const auto wall = Clock::now();
    uint64_t checksum = 0;
    for (size_t index = 0; index < count; ++index) {
        const auto begin = Clock::now();
        checksum += operation<Kind>(bytes, parent);
        samples[index] = nanoseconds(begin);
    }
    const uint64_t elapsed = nanoseconds(wall);
    observed.fetch_add(checksum, std::memory_order_relaxed);
    output(scenario, repetition, 1, bytes, count, elapsed, std::move(samples), before, statistics());
}
/** --------------------------------------------------------------------------------------------------------- Clock Cost
 * @brief Reports the unadjusted cost of sampling an otherwise empty interval.
 */
void clock_cost() {
    std::vector<uint64_t> samples(50000);
    const auto before = statistics();
    const auto wall = Clock::now();
    for (auto& sample : samples) {
        const auto begin = Clock::now();
        sample = nanoseconds(begin);
    }
    const uint64_t elapsed = nanoseconds(wall);
    output("clock_pair", 0, 1, 0, samples.size(), elapsed, samples, before, statistics());
}
/** --------------------------------------------------------------------------------------------------------- Rollover
 * @brief Measures whole-slab claims that force advancement through the actual prepared chain.
 */
void rollover(unsigned repetition) {
    ba_stats_t placement;
    ba_stats(ba_placement_default(), &placement);
    constexpr size_t count = 128;
    std::vector<uint64_t> samples(count);
    std::fprintf(stderr, "Rollover slab_bytes=%llu budget_bytes=%llu allocated_bytes=%llu freed_bytes=%llu\n",
        (unsigned long long)placement.slab_bytes, (unsigned long long)placement.budget_bytes,
        (unsigned long long)placement.slab_bytes_allocated, (unsigned long long)placement.slab_bytes_freed);
    const auto before = statistics();
    const auto wall = Clock::now();
    uint64_t checksum = 0;
    size_t succeeded = 0, failures = 0;
    for (size_t index = 0; index < count; ++index) {
        const auto begin = Clock::now();
        ba_slice_t slice;
        const auto status = ba_claim(ba_placement_default(), placement.slab_bytes, 0, &slice);
        if (status != BA_OK) {
            std::fprintf(stderr, "Rollover failed after %zu completed claims: %s\n", succeeded, ba_status_name(status));
            failures = 1;
            failed_operations = true;
            break;
        }
        auto* bytes = static_cast<volatile unsigned char*>(slice.ptr);
        bytes[0] = 17;
        bytes[placement.slab_bytes - 1] = 31;
        checksum += bytes[0] + bytes[placement.slab_bytes - 1];
        ba_release(&slice);
        samples[succeeded++] = nanoseconds(begin);
    }
    const uint64_t elapsed = nanoseconds(wall);
    samples.resize(succeeded);
    observed.fetch_add(checksum, std::memory_order_relaxed);
    output("whole_slab_touch_drop", repetition, 1, placement.slab_bytes, succeeded, elapsed,
        std::move(samples), before, statistics(), failures);
}
/** --------------------------------------------------------------------------------------------------------- Queue
 * @brief Times real Slice handoffs with payloads claimed before timing and verifies every identity.
 */
void queue(unsigned repetition, size_t producers, size_t consumers) {
    constexpr size_t per_producer = 50000;
    const size_t count = producers * per_producer;
    SliceQueue channel(producers, consumers);
    std::vector<Slice> input;
    input.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        input.emplace_back(64);
        input.back().data<uint64_t>()[0] = index + 1;
        input.back().data<uint64_t>()[1] = (index + 1) ^ 0x9842375ull;
    }
    std::vector<uint64_t> sums(consumers), received(consumers);
    std::vector<std::vector<uint64_t>> identifiers(consumers);
    for (auto& values : identifiers) values.reserve(count);
    std::vector<std::thread> writers, readers;
    std::barrier ready(static_cast<std::ptrdiff_t>(producers + consumers + 1));
    std::barrier start(static_cast<std::ptrdiff_t>(producers + consumers + 1));
    std::atomic<bool> valid{true};
    for (size_t consumer = 0; consumer < consumers; ++consumer) {
        readers.emplace_back([&, consumer] {
            auto reader = channel.consumer(consumer);
            ready.arrive_and_wait();
            start.arrive_and_wait();
            Slice slice;
            while (reader.pop(slice)) {
                const uint64_t identifier = slice.data<uint64_t>()[0];
                if (slice.data<uint64_t>()[1] != (identifier ^ 0x9842375ull)) valid.store(false);
                sums[consumer] += identifier;
                identifiers[consumer].push_back(identifier);
                ++received[consumer];
            }
        });
    }
    for (size_t producer = 0; producer < producers; ++producer) {
        writers.emplace_back([&, producer] {
            auto writer = channel.producer(producer);
            ready.arrive_and_wait();
            start.arrive_and_wait();
            for (size_t index = producer * per_producer; index < (producer + 1) * per_producer; ++index)
                writer.push(std::move(input[index]));
            writer.flush();
        });
    }
    ready.arrive_and_wait();
    const auto before = statistics();
    const auto begin = Clock::now();
    start.arrive_and_wait();
    for (auto& writer : writers) writer.join();
    channel.close();
    for (auto& reader : readers) reader.join();
    const uint64_t elapsed = nanoseconds(begin);
    const auto after = statistics();
    uint64_t sum = 0, total = 0;
    std::vector<uint64_t> all;
    all.reserve(count);
    for (size_t consumer = 0; consumer < consumers; ++consumer) {
        sum += sums[consumer];
        total += received[consumer];
        all.insert(all.end(), identifiers[consumer].begin(), identifiers[consumer].end());
    }
    std::sort(all.begin(), all.end());
    for (size_t index = 0; index < all.size(); ++index)
        require(all[index] == index + 1, "Queue lost or duplicated an identifier");
    require(valid.load() && total == count && sum == count * (count + 1) / 2,
        "Queue benchmark failed payload validation");
    output(producers == 1 ? "queue_1_to_1" : "queue_4_to_4", repetition, producers + consumers,
        64, count, elapsed, {}, before, after);
}
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs repeated uninstrumented workload measurements and prints CSV to standard output.
 */
int main(int count, char** arguments) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    try {
        const unsigned repetitions = count > 1 ? static_cast<unsigned>(std::stoul(arguments[1])) : 3;
        ba_sysinfo_t machine;
        ba_sysinfo(&machine);
        std::fprintf(stderr, "Arena benchmark: hardware_threads=%u page=%llu cache_line=%u Slice_bytes=%zu repetitions=%u\n",
            machine.hw_threads, (unsigned long long)machine.page, machine.cache_line, sizeof(Slice), repetitions);
        std::puts("scenario,repetition,threads,payload_bytes,operations,elapsed_ns,ns_per_operation,million_operations_per_second,p50_ns,p95_ns,p99_ns,max_ns,slab_bytes_allocated,novel_bytes_allocated,failed_operations,latency_samples");
        clock_cost();
        for (unsigned repetition = 1; repetition <= repetitions; ++repetition) {
            std::fprintf(stderr, "Measuring arena repetition %u/%u\n", repetition, repetitions);
            for (size_t threads : {1u, 2u, 4u, 8u, 16u, 32u}) {
                throughput<Operation::claim>("claim_touch_drop", repetition, threads, 64, 250000);
                throughput<Operation::claim>("claim_touch_drop", repetition, threads, 1024, 100000);
                throughput<Operation::copy>("copy_drop", repetition, threads, 64, 100000);
                throughput<Operation::view>("view_drop", repetition, threads, 64, 100000);
            }
            throughput<Operation::claim>("claim_touch_drop", repetition, 1, 65536, 10000);
            throughput<Operation::aligned>("aligned_zero_free", repetition, 1, 64, 250000);
            throughput<Operation::aligned>("aligned_zero_free", repetition, 1, 1024, 100000);
            latency<Operation::claim>("claim_touch_drop", repetition, 64, 50000);
            latency<Operation::claim>("claim_touch_drop", repetition, 1024, 50000);
            latency<Operation::claim>("claim_touch_drop", repetition, 65536, 5000);
            latency<Operation::novel>("novel_touch_drop", repetition, 1024, 10000);
            rollover(repetition);
            queue(repetition, 1, 1);
            queue(repetition, 4, 4);
        }
        std::fprintf(stderr, "Arena benchmark complete: checksum=%llu\n", (unsigned long long)observed.load());
        BuffetMenu::shutdown();
        return failed_operations ? 1 : 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Benchmark failed: %s\n", error.what());
        return 1;
    }
}
