/** --------------------------------------------------------------------------------------------------------- Claim Table Benchmark
 * @file claim_table_benchmark.cpp
 * @brief Claims 1 GiB per size and thread count through short-lived claims and prints one standard table.
 */
#include "benchmark_support.hpp"
#include <algorithm>
#include <array>
#include <barrier>
#include <charconv>
#include <iomanip>
#include <sstream>
#include <thread>
#include <vector>

namespace {
using benchmarks::Clock;
using benchmarks::Progress;
using benchmarks::require;
using buffetalligator::BuffetDescriptor;
using buffetalligator::BuffetDescriptors;
using buffetalligator::Slice;
constexpr size_t TOTAL_BYTES = size_t{1} << 30;
constexpr std::array<size_t, 4> SIZES{64, 128, 4096, size_t{64} << 20};
constexpr std::array<size_t, 2> THREADS{1, 16};
constexpr size_t WINDOW = 64;  ///< Claims each thread keeps alive; each new claim releases the oldest.
/** --------------------------------------------------------------------------------------------------------- Workload
 * @brief Gives each thread a short window of live claims so releases happen as they would in use.
 */
struct Workload {
    const BuffetDescriptor* placement;
    size_t bytes;
    size_t threads;
    size_t claims;
    std::vector<std::vector<Slice>> windows;
    std::barrier<> start;
    Workload(const BuffetDescriptor* selected, size_t size, size_t workers)
    : placement(selected), bytes(size), threads(workers), claims(TOTAL_BYTES / size),
      windows(workers, std::vector<Slice>(WINDOW)), start(static_cast<std::ptrdiff_t>(workers + 1)) {}
    /** ------------------------------------------------------------------------------------------- Claim Range
     * @brief Claims one thread's even share, releasing the claim made a window earlier each time.
     */
    static void claim_range(Workload* work, size_t worker) {
        const size_t share = work->claims * (worker + 1) / work->threads
            - work->claims * worker / work->threads;
        std::vector<Slice>& window = work->windows[worker];
        work->start.arrive_and_wait();
        for (size_t index = 0; index < share; ++index) {
            window[index % WINDOW] = Slice(work->bytes, work->placement);
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Sample
 * @brief Times one sample from the shared start to the last thread's finish, then checks and releases.
 */
double sample(const BuffetDescriptor* placement, size_t bytes, size_t threads) {
    Workload work(placement, bytes, threads);
    std::vector<std::thread> workers;
    for (size_t worker = 0; worker < threads; ++worker)
        workers.emplace_back(&Workload::claim_range, &work, worker);
    work.start.arrive_and_wait();
    const Clock::time_point began = Clock::now();
    for (auto& worker : workers) worker.join();
    const double seconds = std::chrono::duration<double>(Clock::now() - began).count();
    for (const auto& window : work.windows) {
        for (const Slice& claim : window) {
            require(claim.is_null() || claim.size_bytes() >= bytes, "A claim came back short.");
        }
    }
    return seconds;
}
/** --------------------------------------------------------------------------------------------------------- Duration
 * @brief Formats a duration with a unit that keeps three significant figures readable.
 */
std::string duration(double seconds) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(2);
    if (seconds < 1e-6) text << seconds * 1e9 << " ns";
    else if (seconds < 1e-3) text << seconds * 1e6 << " us";
    else if (seconds < 1) text << seconds * 1e3 << " ms";
    else text << seconds << " s";
    return text.str();
}
/** --------------------------------------------------------------------------------------------------------- Size Name
 * @brief Names a request size in B, KiB, or MiB.
 */
std::string size_name(size_t bytes) {
    if (bytes >= size_t{1} << 20) return std::to_string(bytes >> 20) + " MiB";
    if (bytes >= size_t{1} << 10) return std::to_string(bytes >> 10) + " KiB";
    return std::to_string(bytes) + " B";
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs one warmup and five samples per row and prints the median of each row.
 */
int main(int count, char** arguments) {
    try {
        size_t repetitions = 5;
        for (int index = 1; index < count; ++index) {
            const std::string_view option(arguments[index]);
            if (option == "--untracked") continue;
            require(option == "--repetitions" && index + 1 < count, "Usage: --repetitions N --untracked");
            const std::string_view value(arguments[++index]);
            require(std::from_chars(value.data(), value.data() + value.size(), repetitions).ec
                == std::errc{} && repetitions > 0, "--repetitions takes a positive integer");
        }
        const BuffetDescriptor* placement = Slice::default_placement();
        LOG_INFO_STREAM << "Claim table: 1 GiB of claims per row, 64 live per thread, on " << placement->type_name_fn()
            << ", median of " << repetitions << " samples after one warmup"
            << ", allocation tracking on";
        std::ostringstream table;
        table << "\n| Size | Threads | Per claim (average) | Claims | Wall time | Total rate |\n"
            << "| --- | ---: | ---: | ---: | ---: | ---: |\n";
        for (const size_t threads : THREADS) {
            for (const size_t bytes : SIZES) {
                Progress progress(size_name(bytes) + " claims on " + std::to_string(threads)
                    + " threads", 600);
                std::vector<double> seconds;
                for (size_t index = 0; index <= repetitions; ++index) {
                    const double elapsed = sample(placement, bytes, threads);
                    if (index) seconds.push_back(elapsed);
                }
                std::sort(seconds.begin(), seconds.end());
                const double median = seconds[seconds.size() / 2];
                const size_t claims = TOTAL_BYTES / bytes;
                std::ostringstream rate;
                rate << std::fixed << std::setprecision(2) << double(claims) / median / 1e6
                    << " M claims/s, " << double(TOTAL_BYTES) / median / 1e9 << " GB/s";
                table << "| " << size_name(bytes) << " | " << threads << " | "
                    << duration(median / double(claims)) << " | " << claims << " | "
                    << duration(median) << " | " << rate.str() << " |\n";
                LOG_INFO_STREAM << size_name(bytes) << " on " << threads << " threads: "
                    << duration(median) << " (range " << duration(seconds.front()) << " to "
                    << duration(seconds.back()) << ")";
            }
        }
        LOG_INFO_STREAM << table.str();
        return 0;
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Claim table benchmark failed: " << error.what();
        return 1;
    }
}
