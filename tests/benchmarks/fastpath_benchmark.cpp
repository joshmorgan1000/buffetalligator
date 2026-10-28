/** --------------------------------------------------------------------------------------------------------- Fast Path Benchmark
 * @file fastpath_benchmark.cpp
 * @brief Times the fast path: claims and releases on one thread and on every hardware thread, copies, views, and
 * when a device is present, shader dispatches one at a time and back to back over the two slots.
 *
 * Usage: buffetalligator_fastpath_benchmark [--claims N] [--dispatches N] [--words N]
 */
#include <alligator.hpp>
#include <alligator/easygpu.hpp>
#include <logging.hpp>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <latch>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using buffetalligator::Slice;
using Clock = std::chrono::steady_clock;
/** --------------------------------------------------------------------------------------------------------- Options
 * @brief The amount of work per measurement.
 */
struct Options {
    size_t claims = 1u << 20;     ///< Claims per thread per measurement.
    size_t dispatches = 256;      ///< Dispatches per GPU measurement.
    uint32_t words = 1u << 16;    ///< Words each dispatch transforms.
};
/** --------------------------------------------------------------------------------------------------------- Parse
 * @brief Reads the sizes from the command line.
 */
Options parse(int count, char** arguments) {
    Options options;
    for (int index = 1; index + 1 < count; index += 2) {
        const std::string_view name(arguments[index]);
        const std::string_view value(arguments[index + 1]);
        size_t number = 0;
        std::from_chars(value.data(), value.data() + value.size(), number);
        if (name == "--claims") options.claims = number;
        else if (name == "--dispatches") options.dispatches = number;
        else if (name == "--words") options.words = static_cast<uint32_t>(number);
    }
    return options;
}
/** --------------------------------------------------------------------------------------------------------- Nanoseconds Per
 * @brief Elapsed nanoseconds per operation.
 */
double per(const Clock::time_point& start, size_t operations) {
    return std::chrono::duration<double, std::nano>(Clock::now() - start).count() / static_cast<double>(operations);
}
/** --------------------------------------------------------------------------------------------------------- Claim Loop
 * @brief Claims and releases `count` 256-byte Slices, sixteen at a time, on the calling thread.
 */
void claim_loop(size_t count) {
    Slice kept[16];
    for (size_t index = 0; index < count; ++index) kept[index & 15] = Slice(256);
}
/** --------------------------------------------------------------------------------------------------------- Params
 * @brief The record a dispatch row reads.
 */
struct Params {
    uint64_t source;
    uint64_t sink;
    uint32_t count;
};
/// @brief The dispatch body: sink[i] = source[i] + 1 over the row's record.
constexpr std::string_view BODY = R"glsl(
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(buffer_reference, std430, buffer_reference_align = 8) buffer Params { uint64_t source; uint64_t sink; uint count; };
void alligator_main(uint slice) {
    Params params = Params(slice_address(slice));
    const uint index = gl_GlobalInvocationID.x;
    if (index >= params.count) return;
    U32Array source = U32Array(params.source);
    U32Array sink = U32Array(params.sink);
    sink.v[index] = source.v[index] + 1u;
}
)glsl";
/** --------------------------------------------------------------------------------------------------------- Done
 * @brief A dispatch's completion: one count off the latch.
 */
void done(std::shared_ptr<void> context) { static_cast<std::latch*>(context.get())->count_down(); }
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs each measurement and reports nanoseconds per operation.
 */
int main(int count, char** arguments) {
    const Options options = parse(count, arguments);
    LOG_INFO_STREAM << "Fast path benchmark on " << (buffetalligator::GPU::exists()
        ? buffetalligator::GPU::device_name() : std::string("heap buffers, no device"));
    claim_loop(options.claims / 8);
    {
        const Clock::time_point start = Clock::now();
        claim_loop(options.claims);
        LOG_INFO_STREAM << "Claim and release, one thread: " << per(start, options.claims) << " ns per claim";
    }
    {
        const size_t threads = std::max(2u, std::thread::hardware_concurrency());
        std::vector<std::thread> workers;
        const Clock::time_point start = Clock::now();
        for (size_t thread = 0; thread < threads; ++thread) workers.emplace_back(&claim_loop, options.claims);
        for (std::thread& worker : workers) worker.join();
        LOG_INFO_STREAM << "Claim and release, " << threads << " threads: "
            << per(start, options.claims * threads) << " ns per claim across threads";
    }
    {
        Slice source(4096);
        const Clock::time_point start = Clock::now();
        for (size_t index = 0; index < options.claims; ++index) { Slice copy(source); }
        LOG_INFO_STREAM << "Copy and release: " << per(start, options.claims) << " ns per copy";
    }
    {
        Slice source(4096);
        const Clock::time_point start = Clock::now();
        for (size_t index = 0; index < options.claims; ++index) { Slice view = source.slice(64, 128); }
        LOG_INFO_STREAM << "View and release: " << per(start, options.claims) << " ns per view";
    }
    if (!buffetalligator::GPU::exists()) return 0;
    const uint32_t groups_x = (options.words + 63u) / 64u;
    buffetalligator::Shader shader(BODY, "fastpath_benchmark", groups_x);
    Slice source(size_t{options.words} * sizeof(uint32_t));
    Slice sink(size_t{options.words} * sizeof(uint32_t));
    Slice record(sizeof(Params));
    Params* params = record.data<Params>();
    params->source = source.address();
    params->sink = sink.address();
    params->count = options.words;
    shader(record).get();
    {
        const Clock::time_point start = Clock::now();
        for (size_t index = 0; index < options.dispatches; ++index) shader(record).get();
        LOG_INFO_STREAM << "Dispatch and wait, one at a time: " << per(start, options.dispatches) / 1e3
            << " us per dispatch over " << options.words << " words";
    }
    {
        std::latch retired(static_cast<std::ptrdiff_t>(options.dispatches));
        const std::shared_ptr<void> latch(std::shared_ptr<std::latch>(), &retired);
        std::vector<buffetalligator::ShaderResult> results;
        results.reserve(options.dispatches);
        const Clock::time_point start = Clock::now();
        for (size_t index = 0; index < options.dispatches; ++index) results.push_back(shader(record, &done, latch));
        retired.wait();
        LOG_INFO_STREAM << "Dispatch back to back over two slots: " << per(start, options.dispatches) / 1e3
            << " us per dispatch over " << options.words << " words";
    }
    {
        Slice list(options.dispatches * sizeof(Slice));
        Slice* ids = list.data<Slice>();
        for (size_t index = 0; index < options.dispatches; ++index) new (ids + index) Slice(record);
        const Clock::time_point start = Clock::now();
        shader(list, options.dispatches).get();
        LOG_INFO_STREAM << "One dispatch of " << options.dispatches << " rows: " << per(start, options.dispatches) / 1e3
            << " us per row over " << options.words << " words";
        for (size_t index = 0; index < options.dispatches; ++index) ids[index].~Slice();
    }
    return 0;
}
