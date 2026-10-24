/** --------------------------------------------------------------------------------------------------------- Slab Churn Benchmark
 * @file slab_churn_benchmark.cpp
 * @brief Measures normal-operation claims that roll through 64 MiB slabs and checks that no buffer
 * outlives its Chomps, with allocation tracking on or off.
 */
#include "benchmark_support.hpp"
#include <alligator/easygpu.hpp>
#include <memory/tracker.hpp>
#include <array>
#include <atomic>
#include <barrier>
#include <charconv>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace {
using benchmarks::Clock;
using benchmarks::Progress;
using benchmarks::require;
using buffetalligator::AlignedHeapBuffer;
using buffetalligator::BuffetDescriptor;
using buffetalligator::BuffetDescriptors;
using buffetalligator::GPU;
using buffetalligator::Memory;
using buffetalligator::Chomp;
constexpr size_t SLAB_BYTES = size_t{64} * 1024 * 1024;
/// @brief Request sizes each worker cycles from its own phase, mostly small with rare large ones.
constexpr std::array<size_t, 16> SIZES{
    64, 64, 128, 192, 256, 512, 1024, 64, 128, 2048, 4096, 256, 64, 16384, 512, 65536};
/** --------------------------------------------------------------------------------------------------------- Average Size
 * @brief Returns the mean request size of the cycled pattern.
 */
constexpr size_t average_size() {
    size_t total = 0;
    for (const size_t bytes : SIZES) total += bytes;
    return total / SIZES.size();
}
/** --------------------------------------------------------------------------------------------------------- Configuration
 * @brief Selects workers, claimed volume, per-worker working set, samples, and tracking.
 */
struct Configuration {
    size_t workers = 8;  ///< Concurrent claiming threads.
    size_t gib = 4;  ///< GiB claimed per sample across all workers.
    size_t size = 0;  ///< Fixed request bytes; zero cycles the mixed pattern.
    size_t live = 1024;  ///< Chomps each worker keeps alive in its ring.
    size_t repetitions = 5;  ///< Measured samples per pass.
    size_t warmup = 1;  ///< Unmeasured samples before each pass.
    size_t timeout = 300;  ///< Seconds before a stalled sample aborts.
    size_t factory_delay_us = 0;  ///< Models a slower device allocator in the counted placement.
    bool untracked = false;  ///< Turns allocation tracking off before the arena starts.
    bool counted = true;  ///< Runs the independently counted heap placement.
    bool device = true;  ///< Runs the default device placement.
};
/** --------------------------------------------------------------------------------------------------------- Parse
 * @brief Reads unsigned options and the placement and tracking switches.
 */
Configuration parse(int count, char** arguments) {
    Configuration config;
    for (int index = 1; index < count; ++index) {
        const std::string_view argument(arguments[index]);
        if (argument == "--untracked") { config.untracked = true; continue; }
        if (argument == "--counted-only") { config.device = false; continue; }
        if (argument == "--device-only") { config.counted = false; continue; }
        require(index + 1 < count, "An option is missing its value.");
        const std::string_view value(arguments[++index]);
        size_t number = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size(),
            "Options take unsigned integers.");
        if (argument == "--workers") config.workers = number;
        else if (argument == "--gib") config.gib = number;
        else if (argument == "--size") config.size = number;
        else if (argument == "--live") config.live = number;
        else if (argument == "--repetitions") config.repetitions = number;
        else if (argument == "--warmup") config.warmup = number;
        else if (argument == "--timeout") config.timeout = number;
        else if (argument == "--factory-delay-us") config.factory_delay_us = number;
        else throw std::runtime_error("Unknown option: " + std::string(argument));
    }
    require(config.workers && config.gib && config.live && config.repetitions,
        "Workers, GiB, live Chomps, and repetitions must be positive.");
    return config;
}
/** --------------------------------------------------------------------------------------------------------- Counted Placement
 * @brief A 64 MiB heap placement that reports to the tracker and also counts its own buffers.
 */
struct alignas(16) Counted {
    static inline std::atomic<int64_t> live_buffers{0};
    static inline std::atomic<int64_t> live_bytes{0};
    static inline std::atomic<uint64_t> opened{0};
    void* memory = nullptr;  ///< This counted buffer's zero-filled heap storage.
    size_t bytes = 0;        ///< Its size.
    static inline std::chrono::microseconds delay{0};  ///< Added to every allocation.
    static inline thread_local uint64_t hooked = 0;  ///< Allocations and frees run on this thread.
    static inline std::atomic<bool> timing{true};  ///< Off once main returns and its statics are gone.
    static inline std::mutex timings_mutex;  ///< Guards the rare allocation and free timings.
    static inline std::vector<uint32_t> allocations;  ///< Allocation durations in nanoseconds.
    static inline std::vector<uint32_t> frees;  ///< Free durations in nanoseconds.
    /** ------------------------------------------------------------------------------------------- Record Hook
     * @brief Marks this thread as having run a buffer hook and keeps the hook's duration.
     */
    static void record_hook(std::vector<uint32_t>& durations, Clock::time_point began) {
        if (!timing.load(std::memory_order_relaxed)) return;
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - began);
        ++hooked;
        std::lock_guard lock(timings_mutex);
        durations.push_back(static_cast<uint32_t>(elapsed.count()));
    }
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Allocates zeroed heap storage and counts it independently of the tracker.
     */
    Counted(size_t size = 0) : bytes((size + 63) & ~size_t{63}) {
        if (size == 0) { bytes = 0; return; }
        const Clock::time_point began = Clock::now();
        if (delay.count()) std::this_thread::sleep_for(delay);
        memory = std::aligned_alloc(64, bytes);
        require(memory != nullptr, "Allocating a counted buffer failed.");
        std::memset(memory, 0, bytes);
        live_buffers.fetch_add(1, std::memory_order_relaxed);
        live_bytes.fetch_add(static_cast<int64_t>(bytes), std::memory_order_relaxed);
        opened.fetch_add(1, std::memory_order_relaxed);
        record_hook(allocations, began);
    }
    Counted(const Counted&) = delete;
    Counted& operator=(const Counted&) = delete;
    Counted(Counted&& other) noexcept
    : memory(std::exchange(other.memory, nullptr)), bytes(std::exchange(other.bytes, 0)) {}
    Counted& operator=(Counted&& other) noexcept {
        std::swap(memory, other.memory); std::swap(bytes, other.bytes); return *this;
    }
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Uncounts the buffer before releasing its storage.
     */
    ~Counted() {
        if (!memory) return;
        const Clock::time_point began = Clock::now();
        live_buffers.fetch_sub(1, std::memory_order_relaxed);
        live_bytes.fetch_sub(static_cast<int64_t>(bytes), std::memory_order_relaxed);
        std::free(memory);
        record_hook(frees, began);
    }
    operator bool() const { return memory != nullptr; }
    bool operator==(std::nullptr_t) const { return memory == nullptr; }
    static void deleter_impl(void* handle) { delete static_cast<Counted*>(handle); }
    static void (*deleter())(void*) { return deleter_impl; }
    static void* host_ptr(void* handle) { return static_cast<Counted*>(handle)->memory; }
    static size_t size_of(void* handle) { return static_cast<Counted*>(handle)->bytes; }
    static void* factory(size_t size) { return new Counted(size); }
    static size_t default_size() { return SLAB_BYTES; }
    static size_t* type_idx() { static size_t idx = 0; return &idx; }
    static const char* type_name() { return "SlabChurnCounted"; }
    static uint64_t device_address(void*) { return 0; }
    size_t size() const { return bytes; }
    void* raw() { return memory; }
    const void* raw() const { return memory; }
    /** ------------------------------------------------------------------------------------------- Descriptor
     * @brief The registered descriptor, created on first use.
     */
    static const BuffetDescriptor* descriptor() { return BuffetDescriptors::descriptor<Counted>(); }
};
/** --------------------------------------------------------------------------------------------------------- Worker Results
 * @brief Keeps one worker's latencies and overlap flag on their own cache lines.
 */
struct alignas(128) WorkerResults {
    std::vector<uint32_t> claims;  ///< Claims that ran no buffer allocation or free, in nanoseconds.
    std::vector<uint32_t> releases;  ///< Releases that ran no buffer free, in nanoseconds.
    std::vector<uint32_t> hooked_claims;  ///< Claims that allocated or freed a counted buffer.
    std::vector<uint32_t> hooked_releases;  ///< Releases that freed a counted buffer.
    bool overlapped = false;  ///< A live Chomp lost its marker to another claim.
};
/** --------------------------------------------------------------------------------------------------------- Workload
 * @brief Each worker keeps a ring of live Chomps, replacing the oldest with every claim.
 */
struct Workload {
    const BuffetDescriptor* placement;
    size_t live;
    size_t size;
    size_t claims_per_worker;
    std::vector<std::vector<Chomp>> rings;
    std::vector<WorkerResults> results;
    std::barrier<> start;
    Workload(const BuffetDescriptor* selected, const Configuration& config, size_t claims)
    : placement(selected), live(config.live), size(config.size), claims_per_worker(claims),
      rings(config.workers, std::vector<Chomp>(config.live)), results(config.workers),
      start(static_cast<std::ptrdiff_t>(config.workers + 1)) {}
    static uint64_t marker(size_t worker, size_t claim) {
        return (uint64_t(worker + 1) << 40) | claim;
    }
    /** ------------------------------------------------------------------------------------------- Churn
     * @brief Claims, marks, checks, and releases through one worker's ring.
     */
    template<bool Timed>
    static void churn(Workload* work, size_t worker) {
        std::vector<Chomp>& ring = work->rings[worker];
        WorkerResults& results = work->results[worker];
        work->start.arrive_and_wait();
        for (size_t claim = 0; claim < work->claims_per_worker; ++claim) {
            Chomp& slot = ring[claim % work->live];
            if (slot && slot.get_as<uint64_t>() != marker(worker, claim - work->live))
                results.overlapped = true;
            const size_t bytes = work->size ? work->size
                : SIZES[(claim + worker * 5) % SIZES.size()];
            Clock::time_point began;
            if constexpr (Timed) {
                Counted::hooked = 0;
                began = Clock::now();
            }
            Chomp fresh(bytes, work->placement);
            if constexpr (Timed) {
                using std::chrono::nanoseconds;
                const Clock::time_point claimed = Clock::now();
                const bool claim_hooked = Counted::hooked != 0;
                Counted::hooked = 0;
                slot = std::move(fresh);
                const Clock::time_point released = Clock::now();
                const auto claim_ns = static_cast<uint32_t>(
                    std::chrono::duration_cast<nanoseconds>(claimed - began).count());
                const auto release_ns = static_cast<uint32_t>(
                    std::chrono::duration_cast<nanoseconds>(released - claimed).count());
                (claim_hooked ? results.hooked_claims : results.claims).push_back(claim_ns);
                (Counted::hooked ? results.hooked_releases : results.releases).push_back(release_ns);
            } else {
                slot = std::move(fresh);
            }
            slot.get_as<uint64_t>() = marker(worker, claim);
            slot.data<uint8_t>()[bytes - 1] = 0x5A;
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Percentile
 * @brief Returns the value at a fraction of a sorted latency list.
 */
uint32_t percentile(const std::vector<uint32_t>& sorted, double fraction) {
    const size_t index = static_cast<size_t>(fraction * double(sorted.size()));
    return sorted[std::min(sorted.size() - 1, index)];
}
/** --------------------------------------------------------------------------------------------------------- Describe
 * @brief Sorts a latency list and formats its count and percentiles in nanoseconds.
 */
std::string describe(std::vector<uint32_t>& values) {
    if (values.empty()) return "none";
    std::sort(values.begin(), values.end());
    return "n=" + std::to_string(values.size()) + " p50=" + std::to_string(percentile(values, 0.5))
        + " p99=" + std::to_string(percentile(values, 0.99)) + " p99.9="
        + std::to_string(percentile(values, 0.999)) + " p99.99="
        + std::to_string(percentile(values, 0.9999)) + " max=" + std::to_string(values.back());
}
/** --------------------------------------------------------------------------------------------------------- Live State
 * @brief Snapshots every leak signal available for one placement after its rings drain.
 */
struct LiveState {
    int64_t counted_buffers;
    uint64_t tracked_bytes;
    std::optional<uint64_t> device_bytes;
};
/** --------------------------------------------------------------------------------------------------------- Snapshot
 * @brief Reads the independent buffer count, the tracker, and the native device counter.
 */
LiveState snapshot(const BuffetDescriptor& placement) {
    return {Counted::live_buffers.load(std::memory_order_acquire),
        Memory::placement_usage(placement), std::optional<uint64_t>(GPU::memory_usage().available_bytes)};
}
/** --------------------------------------------------------------------------------------------------------- Run
 * @brief Runs warmups and measured samples of one pass and checks the live state after each.
 */
template<bool Timed>
void run(const Configuration& config, const BuffetDescriptor* placement, const char* name) {
    const size_t claims = (config.gib << 30) / (config.size ? config.size : average_size())
        / config.workers;
    const bool counted = placement == Counted::descriptor();
    std::vector<double> seconds;
    std::vector<uint32_t> claim_latencies;
    std::vector<uint32_t> release_latencies;
    std::vector<uint32_t> hooked_claims;
    std::vector<uint32_t> hooked_releases;
    std::optional<LiveState> settled;
    uint64_t opened_total = 0;
    for (size_t sample = 0; sample < config.warmup + config.repetitions; ++sample) {
        const std::string label = std::string(name) + (Timed ? " latency " : " throughput ")
            + (sample < config.warmup ? "warmup " : "sample ")
            + std::to_string(sample < config.warmup ? sample + 1 : sample - config.warmup + 1);
        Progress progress(label, config.timeout);
        Workload work(placement, config, claims);
        if constexpr (Timed) {
            for (auto& results : work.results) {
                results.claims.reserve(claims);
                results.releases.reserve(claims);
            }
        }
        std::vector<std::thread> workers;
        for (size_t worker = 0; worker < config.workers; ++worker)
            workers.emplace_back(&Workload::churn<Timed>, &work, worker);
        const uint64_t opened_before = counted ? Counted::opened.load(std::memory_order_relaxed)
            : Memory::placement_allocations(*placement) / SLAB_BYTES;
        if (sample == config.warmup) {
            std::lock_guard lock(Counted::timings_mutex);
            Counted::allocations.clear();
            Counted::frees.clear();
        }
        work.start.arrive_and_wait();
        const Clock::time_point began = Clock::now();
        for (auto& worker : workers) worker.join();
        const double elapsed = std::chrono::duration<double>(Clock::now() - began).count();
        for (const auto& results : work.results)
            require(!results.overlapped, "A live Chomp lost its marker to another claim.");
        work.rings.clear();
        const LiveState state = snapshot(*placement);
        if (counted) {
            require(state.counted_buffers == 3,
                "The counted placement held buffers beyond its current buffer and the two ahead.");
        }
        if (settled) {
            /// Tracked bytes may still grow between samples: the claim cursor touches new slot regions, and
            /// those tables come from the tracked placement too.
            require(state.counted_buffers == settled->counted_buffers
                && state.device_bytes == settled->device_bytes,
                "Live buffers grew between samples after every Chomp was released.");
        } else {
            settled = state;
        }
        if (sample < config.warmup) continue;
        seconds.push_back(elapsed);
        opened_total += (counted ? Counted::opened.load(std::memory_order_relaxed)
            : Memory::placement_allocations(*placement) / SLAB_BYTES) - opened_before;
        for (const auto& results : work.results) {
            claim_latencies.insert(claim_latencies.end(), results.claims.begin(),
                results.claims.end());
            release_latencies.insert(release_latencies.end(), results.releases.begin(),
                results.releases.end());
            hooked_claims.insert(hooked_claims.end(), results.hooked_claims.begin(),
                results.hooked_claims.end());
            hooked_releases.insert(hooked_releases.end(), results.hooked_releases.begin(),
                results.hooked_releases.end());
        }
    }
    std::sort(seconds.begin(), seconds.end());
    const double median = seconds[seconds.size() / 2];
    const double total_claims = double(claims * config.workers);
    LOG_INFO_STREAM << name << (Timed ? " latency" : " throughput") << " workers=" << config.workers
        << " tracking=on"
        << ": median " << median * 1000 << " ms per " << config.gib << " GiB, "
        << total_claims / median / 1e6 << " M claims/s, " << double(config.gib << 30) / median / 1e9
        << " GB/s claimed, " << opened_total / config.repetitions << " slabs opened per sample";
    if constexpr (Timed) {
        const char* scope = counted ? " without a buffer allocation or free" : " including device buffers";
        LOG_INFO_STREAM << name << " claims" << scope << ", ns: " << describe(claim_latencies);
        LOG_INFO_STREAM << name << " releases" << scope << ", ns: " << describe(release_latencies);
        if (counted) {
            std::lock_guard lock(Counted::timings_mutex);
            LOG_INFO_STREAM << name << " claims that ran a buffer allocation or free, ns: "
                << describe(hooked_claims);
            LOG_INFO_STREAM << name << " releases that freed a buffer, ns: " << describe(hooked_releases);
            LOG_INFO_STREAM << name << " 64 MiB allocations alone, ns: " << describe(Counted::allocations);
            LOG_INFO_STREAM << name << " 64 MiB frees alone, ns: " << describe(Counted::frees);
        }
    }
    LOG_INFO_STREAM << name << " live after draining: counted buffers=" << settled->counted_buffers
        << ", tracked bytes=" << settled->tracked_bytes << ", device bytes="
        << (settled->device_bytes ? std::to_string(*settled->device_bytes) : std::string("n/a"))
        << "; unchanged across all samples";
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs throughput and latency passes on the counted heap and default device placements.
 */
int main(int count, char** arguments) {
    try {
        const Configuration config = parse(count, arguments);
        const BuffetDescriptor* device = Chomp::default_placement();
        Counted::delay = std::chrono::microseconds(config.factory_delay_us);
        LOG_INFO_STREAM << "Slab churn: workers=" << config.workers << ", " << config.gib
            << " GiB claimed per sample, "
            << (config.size ? std::to_string(config.size) + " B" : std::string("mixed 64 B-64 KiB"))
            << " claims, " << config.live << " live Chomps per worker, slab="
            << (SLAB_BYTES >> 20) << " MiB, tracking=on, device placement="
            << device->type_name_fn();
        if (config.counted) {
            run<false>(config, Counted::descriptor(), "counted-heap");
            run<true>(config, Counted::descriptor(), "counted-heap");
        }
        if (config.device) {
            run<false>(config, device, device->type_name_fn());
            run<true>(config, device, device->type_name_fn());
        }
        LOG_INFO_STREAM << "Every sample released all Chomps and left live buffers unchanged";
        Counted::timing.store(false, std::memory_order_relaxed);
        return 0;
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Slab churn benchmark failed: " << error.what();
        Counted::timing.store(false, std::memory_order_relaxed);
        return 1;
    }
}
