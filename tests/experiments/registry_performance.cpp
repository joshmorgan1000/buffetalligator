/** --------------------------------------------------------------------------------------------------------- Registry Benchmark
 * @file bench_registry.cpp
 * @brief Compares a fixed slot free list with atomic page growth and shared-lock access.
 */
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <string_view>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
constexpr uint32_t maximum_slots = 1u << 20;
constexpr uint32_t growth_slots = 1u << 14;
constexpr uint32_t slot_stride = 2;
/** --------------------------------------------------------------------------------------------------------- Require
 * @brief Stops the benchmark when an operation or invariant fails.
 */
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}
/** --------------------------------------------------------------------------------------------------------- Plate
 * @brief Mirrors the core's 64-byte backing record geometry.
 */
struct alignas(64) Plate {
    std::atomic<uint64_t> state{0};
    uint64_t payload[4]{};
    uint32_t placement = 0;
    uint32_t kind = 0;
    std::atomic<uint32_t> next_free{0};
    uint32_t reserved = 0;
};
static_assert(sizeof(Plate) == 64);
/** --------------------------------------------------------------------------------------------------------- Mode
 * @brief Selects the synchronization strategy at compile time.
 */
enum class Mode { fixed, growing, shared };
/** --------------------------------------------------------------------------------------------------------- Registry
 * @brief Models the core's tagged free list and fresh-slot permutation in stable virtual storage.
 */
template<Mode mode>
class Registry {
private:
    std::byte* storage_ = nullptr;
    alignas(128) std::atomic<uint64_t> head_{0};
    alignas(128) std::atomic<uint32_t> fresh_{1};
    alignas(128) std::atomic<uint32_t> capacity_{0};
    std::mutex growth_mutex_;
    std::shared_mutex access_mutex_;
    std::array<double, maximum_slots / growth_slots> growth_times_{};
    size_t growth_count_ = 0;
    /** ------------------------------------------------------------------------------------------- Grow
     * @brief Publishes writable pages before a fresh claimant constructs its record.
     */
    [[gnu::noinline]] void grow(uint32_t slot) {
        std::lock_guard lock(growth_mutex_);
        const uint32_t previous = capacity_.load(std::memory_order_relaxed);
        if (slot < previous) return;
        const auto begin = Clock::now();
        const uint32_t next = (slot / growth_slots + 1) * growth_slots;
        require(next <= maximum_slots, "growth exceeds reserved storage");
        require(mprotect(storage_ + size_t(previous) * sizeof(Plate),
            size_t(next - previous) * sizeof(Plate), PROT_READ | PROT_WRITE) == 0,
            "mprotect growth failed");
        capacity_.store(next, std::memory_order_release);
        growth_times_[growth_count_++] =
            std::chrono::duration<double, std::micro>(Clock::now() - begin).count();
    }
    /** ------------------------------------------------------------------------------------------- Pop
     * @brief Claims a free-list record or constructs a fresh record after checking capacity.
     */
    uint32_t pop_impl() {
        uint64_t head = head_.load(std::memory_order_acquire);
        for (;;) {
            const uint32_t slot = static_cast<uint32_t>(head);
            if (!slot) {
                const uint32_t fresh = fresh_.fetch_add(1, std::memory_order_relaxed);
                require(fresh < maximum_slots, "slot registry exhausted");
                const uint32_t columns = maximum_slots / slot_stride;
                const uint32_t mapped = (fresh % columns) * slot_stride + fresh / columns;
                if constexpr (mode == Mode::growing) {
                    if (mapped >= capacity_.load(std::memory_order_acquire)) grow(mapped);
                }
                std::construct_at(at(mapped));
                return mapped;
            }
            const uint32_t next = at(slot)->next_free.load(std::memory_order_relaxed);
            const uint64_t replacement = (head & 0xffffffff00000000ull) | next;
            if (head_.compare_exchange_weak(head, replacement,
                std::memory_order_acq_rel, std::memory_order_acquire)) return slot;
        }
    }
    /** ------------------------------------------------------------------------------------------- Push
     * @brief Publishes a recycled record with an incremented ABA tag.
     */
    void push_impl(uint32_t slot) {
        uint64_t head = head_.load(std::memory_order_acquire);
        do {
            at(slot)->next_free.store(static_cast<uint32_t>(head), std::memory_order_relaxed);
        } while (!head_.compare_exchange_weak(head,
            ((head + (1ull << 32)) & 0xffffffff00000000ull) | slot,
            std::memory_order_acq_rel, std::memory_order_acquire));
    }
public:
    Registry() { reset(true); }
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    ~Registry() { require(munmap(storage_, size_t(maximum_slots) * sizeof(Plate)) == 0,
        "munmap failed"); }
    /** ------------------------------------------------------------------------------------------- Record
     * @brief Resolves an immutable record address within the reservation.
     */
    Plate* at(uint32_t slot) {
        return reinterpret_cast<Plate*>(storage_ + size_t(slot) * sizeof(Plate));
    }
    /** ------------------------------------------------------------------------------------------- Reset
     * @brief Resets a quiescent trial and optionally replaces its mapping with untouched pages.
     */
    void reset(bool cold) {
        if (cold) {
            if (storage_) require(munmap(storage_, size_t(maximum_slots) * sizeof(Plate)) == 0,
                "munmap reset failed");
            const int protection = mode == Mode::growing ? PROT_NONE : PROT_READ | PROT_WRITE;
            void* mapping = mmap(nullptr, size_t(maximum_slots) * sizeof(Plate),
                protection, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            require(mapping != MAP_FAILED, "mmap reservation failed");
            storage_ = ::new (mapping) std::byte[size_t(maximum_slots) * sizeof(Plate)];
            if constexpr (mode == Mode::growing) {
                require(mprotect(storage_, size_t(growth_slots) * sizeof(Plate),
                    PROT_READ | PROT_WRITE) == 0, "mprotect initial capacity failed");
                capacity_.store(growth_slots, std::memory_order_relaxed);
            } else {
                capacity_.store(maximum_slots, std::memory_order_relaxed);
            }
            std::construct_at(at(0));
        }
        head_.store(0, std::memory_order_relaxed);
        fresh_.store(1, std::memory_order_relaxed);
        growth_count_ = 0;
    }
    /** ------------------------------------------------------------------------------------------- Prepare
     * @brief Makes the complete reservation writable and faults in its pages outside timing.
     */
    void prepare() {
        require(mprotect(storage_, size_t(maximum_slots) * sizeof(Plate),
            PROT_READ | PROT_WRITE) == 0, "mprotect preparation failed");
        const long page_bytes = sysconf(_SC_PAGESIZE);
        require(page_bytes > 0, "page size query failed");
        for (size_t offset = 0; offset < size_t(maximum_slots) * sizeof(Plate);
            offset += static_cast<size_t>(page_bytes)) storage_[offset] = std::byte{0};
        capacity_.store(maximum_slots, std::memory_order_relaxed);
    }
    /** ------------------------------------------------------------------------------------------- Claim
     * @brief Measures optional shared-reader acquisition around the slot claim.
     */
    uint32_t pop() {
        if constexpr (mode == Mode::shared) {
            std::shared_lock lock(access_mutex_);
            return pop_impl();
        } else {
            return pop_impl();
        }
    }
    /** ------------------------------------------------------------------------------------------- Recycle
     * @brief Measures optional shared-reader acquisition around recycling.
     */
    void push(uint32_t slot) {
        if constexpr (mode == Mode::shared) {
            std::shared_lock lock(access_mutex_);
            push_impl(slot);
        } else {
            push_impl(slot);
        }
    }
    /** ------------------------------------------------------------------------------------------- Growth Statistics
     * @brief Copies completed growth-event timings after worker synchronization.
     */
    std::vector<double> growth_times() const {
        return {growth_times_.begin(), growth_times_.begin() + growth_count_};
    }
};
/** --------------------------------------------------------------------------------------------------------- Worker Result
 * @brief Separates worker checksums onto distinct Apple Silicon cache lines.
 */
struct alignas(128) WorkerResult {
    uint64_t checksum = 0;
};
/** --------------------------------------------------------------------------------------------------------- Team
 * @brief Reuses workers across trials with publication and completion barriers.
 */
class Team {
private:
    std::barrier<> barrier_;
    std::vector<std::thread> workers_;
    std::function<void(size_t)> work_;
    bool stopping_ = false;
public:
    explicit Team(size_t count) : barrier_(static_cast<std::ptrdiff_t>(count + 1)) {
        for (size_t index = 0; index < count; ++index) {
            workers_.emplace_back([this, index] {
                for (;;) {
                    barrier_.arrive_and_wait();
                    if (stopping_) return;
                    work_(index);
                    barrier_.arrive_and_wait();
                }
            });
        }
    }
    ~Team() {
        stopping_ = true;
        barrier_.arrive_and_wait();
        for (auto& worker : workers_) worker.join();
    }
    /** ------------------------------------------------------------------------------------------- Run
     * @brief Times a bulk operation batch excluding worker creation and task construction.
     */
    double run(std::function<void(size_t)> work) {
        work_ = std::move(work);
        const auto begin = Clock::now();
        barrier_.arrive_and_wait();
        barrier_.arrive_and_wait();
        return std::chrono::duration<double, std::nano>(Clock::now() - begin).count();
    }
};
/** --------------------------------------------------------------------------------------------------------- Trial
 * @brief Measures fresh claims or claim-and-recycle pairs and emits one CSV sample.
 */
template<Mode mode>
void trial(Team& team, Registry<mode>& registry, const char* label,
    std::string_view scenario, size_t threads, int repetition, size_t requested
) {
    const size_t per_worker = requested / threads;
    const size_t batch_operations = per_worker * threads;
    const size_t batches = scenario == "fresh" ? 16 : 1;
    const size_t operations = batch_operations * batches;
    std::vector<WorkerResult> results(threads);
    const bool recycling = scenario == "recycle";
    double elapsed = 0;
    uint64_t checksum = 0;
    for (size_t batch = 0; batch < batches; ++batch) {
        registry.reset(scenario == "growth");
        if (recycling) {
            std::vector<uint32_t> seeds(threads);
            for (auto& slot : seeds) slot = registry.pop();
            for (auto slot : seeds) registry.push(slot);
        }
        elapsed += team.run([&](size_t worker) {
            uint64_t checksum = 0;
            for (size_t operation = 0; operation < per_worker; ++operation) {
                const uint32_t slot = registry.pop();
                checksum += slot;
                if (recycling) registry.push(slot);
            }
            results[worker].checksum = checksum;
        });
        for (const auto& result : results) checksum += result.checksum;
    }
    require(checksum != 0, "empty benchmark checksum");
    if (!recycling) {
        require(batch_operations < maximum_slots / slot_stride, "fresh sample exceeds first stripe");
        require(checksum == batches * batch_operations * (batch_operations + 1),
            "fresh slots are missing or duplicated");
    }
    auto growth = registry.growth_times();
    std::sort(growth.begin(), growth.end());
    const auto percentile = [&](double fraction) {
        return growth.empty() ? 0.0 : growth[static_cast<size_t>(fraction * (growth.size() - 1))];
    };
    if (repetition >= 0) std::printf("%.*s,%s,%zu,%d,%zu,%.3f,%zu,%.3f,%.3f,%.3f,%llu\n",
        static_cast<int>(scenario.size()), scenario.data(), label, threads, repetition,
        operations, elapsed / operations, growth.size(), percentile(0.5), percentile(0.99),
        percentile(1.0), static_cast<unsigned long long>(checksum));
}
/** --------------------------------------------------------------------------------------------------------- Verify
 * @brief Checks unique slots, concurrent growth, stable live records, and recycled ownership.
 */
void verify(size_t threads) {
    Registry<Mode::growing> registry;
    Team team(threads);
    Plate* sentinel = registry.at(0);
    constexpr size_t claims = 65536;
    const size_t per_worker = claims / threads;
    std::vector<std::vector<uint32_t>> issued(threads, std::vector<uint32_t>(per_worker));
    team.run([&](size_t worker) {
        for (size_t operation = 0; operation < per_worker; ++operation) {
            const uint32_t slot = registry.pop();
            require(registry.at(slot)->state.exchange(1, std::memory_order_acq_rel) == 0,
                "duplicate fresh owner");
            issued[worker][operation] = slot;
            sentinel->state.fetch_add(1, std::memory_order_relaxed);
        }
    });
    require(sentinel == registry.at(0), "growth moved an existing record");
    require(sentinel->state.load() == per_worker * threads, "growth lost live reference updates");
    require(!registry.growth_times().empty(), "verification did not exercise growth");
    std::vector<uint32_t> slots;
    for (const auto& worker : issued) slots.insert(slots.end(), worker.begin(), worker.end());
    std::sort(slots.begin(), slots.end());
    for (size_t index = 0; index < slots.size(); ++index) {
        require(slots[index] == (index + 1) * slot_stride, "fresh slot uniqueness failed");
        registry.at(slots[index])->state.store(0, std::memory_order_relaxed);
        registry.push(slots[index]);
    }
    team.run([&](size_t) {
        for (size_t operation = 0; operation < per_worker; ++operation) {
            const uint32_t slot = registry.pop();
            require(registry.at(slot)->state.exchange(1, std::memory_order_acq_rel) == 0,
                "duplicate recycled owner");
            require(registry.at(slot)->state.exchange(0, std::memory_order_acq_rel) == 1,
                "recycled ownership lost");
            registry.push(slot);
        }
    });
    std::printf("verified threads=%zu claims=%zu growth_events=%zu\n",
        threads, per_worker * threads, registry.growth_times().size());
}
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs rotated trials at one, two, and all hardware threads or oversubscribed verification.
 */
int main(int arguments, char** values) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const size_t hardware = std::thread::hardware_concurrency();
    require(hardware > 0, "hardware thread count unavailable");
    if (arguments == 2 && std::string_view(values[1]) == "--verify") {
        for (size_t threads : {size_t{1}, size_t{2}, hardware * 2}) verify(threads);
        return 0;
    }
    require(arguments == 1, "usage: buffetalligator_registry_bench [--verify]");
    std::fprintf(stderr, "prototype registry: %zu hardware threads, 64-byte records, "
        "64 MiB reservation, 1 MiB growth chunks, stride 2; two warmups and nine trials\n", hardware);
    std::puts("scenario,mode,threads,trial,operations,ns_per_operation,growth_events,"
        "growth_p50_us,growth_p99_us,growth_max_us,checksum");
    for (size_t threads : {size_t{1}, size_t{2}, hardware}) {
        Team team(threads);
        Registry<Mode::fixed> fixed;
        Registry<Mode::growing> growing;
        Registry<Mode::shared> shared;
        fixed.prepare();
        growing.prepare();
        shared.prepare();
        for (std::string_view scenario : {"fresh", "recycle", "growth"}) {
            const size_t operations = scenario == "recycle" ? 4194304 : 262144;
            for (int repetition = -2; repetition < 9; ++repetition) {
                for (int position = 0; position < 3; ++position) {
                    switch ((repetition + 3 + position) % 3) {
                    case 0: trial(team, fixed, "fixed", scenario, threads, repetition, operations); break;
                    case 1: trial(team, growing, "atomic_growth", scenario, threads, repetition, operations); break;
                    case 2:
                        if (scenario != "growth") trial(team, shared, "shared_guard",
                            scenario, threads, repetition, operations);
                        break;
                    }
                }
            }
        }
    }
}
