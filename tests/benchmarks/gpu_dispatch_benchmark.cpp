/** --------------------------------------------------------------------------------------------------------- GPU Dispatch Benchmark
 * @file gpu_dispatch_benchmark.cpp
 * @brief Measures completed native and translated dispatches, owned completions, and program preparation.
 */
#include "benchmark_support.hpp"
#include <alligator/easygpu.hpp>
#include <alligator/easyvulkan.hpp>
#include <alligator/kitchen.hpp>
#include <gpu/runtime.hpp>
#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

namespace {
using namespace benchmarks;
using namespace buffetalligator;
/** --------------------------------------------------------------------------------------------------------- Configuration
 * @brief Configures explicit backend/source identity and bounded persistent submitter workloads.
 */
struct Configuration {
    Options common;
    size_t submitters = 0;
    size_t depth = 8;
    size_t batch = 0;
    std::string backend;
    std::string source = "translated";
    std::string tag;
};
/** --------------------------------------------------------------------------------------------------------- Parse Configuration
 * @brief Reuses common timing options with explicit source and submission controls.
 */
Configuration configuration(int count, char** arguments) {
    Configuration result;
    std::vector<char*> shared{arguments[0]};
    bool items_set = false, repetitions_set = false, warmup_set = false;
    for (int index = 1; index < count; ++index) {
        const std::string_view argument(arguments[index]);
        if (argument == "--source") {
            require(index + 1 < count, "--source requires native or translated.");
            result.source = arguments[++index];
            require(result.source == "native" || result.source == "translated",
                "--source requires native or translated.");
            continue;
        }
        if (argument == "--submitters" || argument == "--depth" || argument == "--batch") {
            require(index + 1 < count, "A GPU benchmark option is missing its value.");
            const std::string_view value(arguments[++index]);
            size_t number = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
            require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()
                && number != 0, "GPU benchmark options require a positive integer.");
            if (argument == "--submitters") result.submitters = number;
            else if (argument == "--depth") result.depth = number;
            else result.batch = number;
            continue;
        }
        require(argument != "--producers" && argument != "--consumers" && argument != "--lookups",
            "Use --submitters, --depth, and --batch for the GPU benchmark.");
        items_set = items_set || argument == "--items";
        repetitions_set = repetitions_set || argument == "--repetitions";
        warmup_set = warmup_set || argument == "--warmup";
        shared.push_back(arguments[index]);
        if (argument != "--help" && argument != "--single-thread") {
            require(index + 1 < count, "An option is missing its value; use --help.");
            shared.push_back(arguments[++index]);
        }
    }
    result.common = parse(static_cast<int>(shared.size()), shared.data(), false);
    if (!items_set) result.common.items = 128;
    if (!repetitions_set) result.common.repetitions = 15;
    if (!warmup_set) result.common.warmup = 2;
    if (const char* selected = std::getenv("ALLIGATOR_GPU_BACKEND")) result.backend = selected;
    result.tag = result.backend + '-' + result.source;
    if (result.common.csv.empty()) result.common.csv = result.tag + "_dispatch_samples.csv";
    require(result.common.repetitions <= SIZE_MAX - result.common.warmup, "Sample count overflow.");
    require(!result.common.single_thread || result.submitters <= 1,
        "Use --single-thread or --submitters N, not both.");
    require(result.common.items <= UINT32_MAX / std::max(size_t(256), result.batch),
        "The requested batch trace exceeds deterministic identifier capacity.");
    return result;
}
/** --------------------------------------------------------------------------------------------------------- Identity Kernel
 * @brief Writes a deterministic marker, identity, and represented length for every stream.
 */
constexpr std::string_view identity_source = R"glsl(
void alligator_main(Slice stream) {
    if (gl_LocalInvocationIndex != 0u) return;
    slice_store_u32(stream, 1u, slice_load_u32(stream, 0u) ^ 0x9e3779b9u);
    slice_store_u32(stream, 2u, stream.id);
    slice_store_u32(stream, 3u, uint(slice_size(stream)));
}
)glsl";
/** --------------------------------------------------------------------------------------------------------- Coroutine Task
 * @brief Cancels retained suspension before the benchmark destroys its owned coroutine frame.
 */
struct CoroutineTask {
    struct promise_type {
        CoroutineTask get_return_object() {
            return CoroutineTask(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };
    std::coroutine_handle<promise_type> frame;
    ShaderResult owner;
    explicit CoroutineTask(std::coroutine_handle<promise_type> handle) : frame(handle) {}
    CoroutineTask(CoroutineTask&& other) noexcept
    : frame(std::exchange(other.frame, {})), owner(std::move(other.owner)) {}
    CoroutineTask(const CoroutineTask&) = delete;
    ~CoroutineTask() {
        if (frame) { owner.cancel(); frame.destroy(); }
    }
};
/** --------------------------------------------------------------------------------------------------------- Dispatch
 * @brief Owns one dispatch's streams, callback context, terminal result, and optional coroutine.
 */
struct Dispatch {
    std::vector<Slice> streams;
    TaskCountdown finished{1};
    ShaderResult result;
    std::optional<CoroutineTask> coroutine;
    std::exception_ptr error;
    Clock::time_point started;
    double nanoseconds = -1;
    double admission_nanoseconds = -1;
    bool instrumented = false;
    /** ------------------------------------------------------------------------------------------- Complete
     * @brief Records host completion observation before publishing the last context access.
     */
    static void complete(void* context) {
        auto& dispatch = *static_cast<Dispatch*>(context);
        if (dispatch.instrumented) {
            dispatch.nanoseconds = std::chrono::duration<double, std::nano>(
                Clock::now() - dispatch.started).count();
        }
        TaskCountdown::arrive(&dispatch.finished);
    }
};
/** --------------------------------------------------------------------------------------------------------- Await Dispatch
 * @brief Retains terminal errors outside its frame and records the actual resumed continuation.
 */
CoroutineTask await_dispatch(ShaderAwaiter awaiter, Dispatch* dispatch) {
    try { co_await std::move(awaiter); }
    catch (...) { dispatch->error = std::current_exception(); }
    Dispatch::complete(dispatch);
}
/** --------------------------------------------------------------------------------------------------------- Workload
 * @brief Reuses bounded in-flight windows across persistent submitters sharing one prepared Shader.
 */
struct Workload {
    Shader& shader;
    size_t workers;
    size_t depth;
    bool awaiter;
    bool instrumented = false;
    std::vector<std::unique_ptr<Dispatch>> dispatches;
    Workload(Shader& program, size_t items, size_t batch, size_t submitters,
        size_t outstanding, bool use_awaiter)
    : shader(program), workers(submitters), depth(outstanding), awaiter(use_awaiter) {
        dispatches.reserve(items);
        for (size_t index = 0; index < items; ++index) {
            auto dispatch = std::make_unique<Dispatch>();
            dispatch->streams.reserve(batch);
            for (size_t stream = 0; stream < batch; ++stream) {
                dispatch->streams.emplace_back(64, gpu_device().placement);
                dispatch->streams.back().get_as<uint32_t>() = uint32_t(index * batch + stream);
            }
            dispatches.push_back(std::move(dispatch));
        }
    }
    /** ------------------------------------------------------------------------------------------- Prepare
     * @brief Resets completed contexts and visible outputs outside the measured pass.
     */
    void prepare() {
        for (auto& dispatch : dispatches) {
            dispatch->coroutine.reset();
            dispatch->result = ShaderResult();
            dispatch->error = {};
            dispatch->finished.rearm(1);
            dispatch->instrumented = instrumented;
            dispatch->nanoseconds = -1;
            dispatch->admission_nanoseconds = -1;
            for (Slice& stream : dispatch->streams) {
                stream.data<uint32_t>()[1] = 0;
                stream.data<uint32_t>()[2] = UINT32_MAX;
                stream.data<uint32_t>()[3] = 0;
            }
        }
    }
    /** ------------------------------------------------------------------------------------------- Run
     * @brief Submits a bounded window and waits for actual callbacks or resumed continuations.
     */
    void run(size_t worker) {
        const size_t begin = partition(dispatches.size(), worker, workers);
        const size_t end = partition(dispatches.size(), worker + 1, workers);
        for (size_t window = begin; window < end;) {
            const size_t finish = window + std::min(depth, end - window);
            for (size_t index = window; index < finish; ++index) {
                Dispatch& dispatch = *dispatches[index];
                if (instrumented) dispatch.started = Clock::now();
                if (awaiter) {
                    auto pending = shader.dispatch(dispatch.streams.data(), dispatch.streams.size());
                    dispatch.result = pending.result();
                    if (instrumented) {
                        dispatch.admission_nanoseconds = std::chrono::duration<double, std::nano>(
                            Clock::now() - dispatch.started).count();
                    }
                    dispatch.coroutine.emplace(await_dispatch(std::move(pending), &dispatch));
                    dispatch.coroutine->owner = dispatch.result;
                    dispatch.coroutine->frame.resume();
                } else {
                    shader(dispatch.streams.data(), dispatch.streams.size(), dispatch.result,
                        &Dispatch::complete, &dispatch);
                    if (instrumented) {
                        dispatch.admission_nanoseconds = std::chrono::duration<double, std::nano>(
                            Clock::now() - dispatch.started).count();
                    }
                }
            }
            for (size_t index = window; index < finish; ++index) dispatches[index]->finished.wait();
            window = finish;
        }
    }
    /** ------------------------------------------------------------------------------------------- Validate
     * @brief Checks terminal success and every stream identity after all timed completions.
     */
    void validate() const {
        for (const auto& dispatch : dispatches) {
            dispatch->result.cancel();
            dispatch->result.rethrow();
            if (dispatch->error) std::rethrow_exception(dispatch->error);
            if (instrumented) {
                require(dispatch->nanoseconds >= 0 && dispatch->admission_nanoseconds >= 0,
                    "An instrumented dispatch has no timestamp sample.");
            }
            for (const Slice& stream : dispatch->streams) {
                const uint32_t* words = stream.data<uint32_t>();
                require(words[1] == (words[0] ^ uint32_t(0x9e3779b9))
                    && words[2] == stream.id() && words[3] == stream.size_bytes(),
                    "GPU dispatch returned incorrect payload, identity, or granule length.");
            }
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Preparation
 * @brief Separates pipeline preparation, source encoding, cold decode, and warm decode-cache lookup.
 */
Shader preparation(const Configuration& config, Reporter& reporter) {
    Progress progress(config.tag + " program preparation", config.common.timeout);
    Options options = config.common;
    options.items = 1;
    options.capacity = 0;
    std::vector<double> compile;
    std::optional<Shader> shader;
    const ShaderSource selected_source = config.source == "native"
        ? ShaderSource{{}, identity_source, {}} : ShaderSource{identity_source, {}, {}};
    for (size_t sample = 0; sample < options.warmup + options.repetitions; ++sample) {
        shader.reset();
        const auto started = Clock::now();
        shader.emplace(selected_source, "gpu_dispatch_benchmark");
        const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
        if (sample >= options.warmup) compile.push_back(seconds);
        if (sample == 0) reporter.report(config.tag, "first-shader-prepare", {1, 0, true},
            options, 1, {seconds});
        Workload check(*shader, 1, 1, 1, 1, false);
        check.prepare();
        check.run(0);
        check.validate();
    }
    reporter.report(config.tag, "shader-prepare", {1, 0, true}, options, 1, std::move(compile));
    std::vector<double> encode;
    Slice encoded;
    for (size_t sample = 0; sample < options.warmup + options.repetitions; ++sample) {
        encoded.free();
        const auto started = Clock::now();
        encoded = GPU::encode(*shader);
        const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
        require(encoded.size_bytes() >= 64 && encoded.data<uint8_t>()[0] == 'B'
            && encoded.data<uint8_t>()[1] == 'A' && encoded.data<uint8_t>()[2] == 'G'
            && encoded.data<uint8_t>()[3] == 'P', "Encoded program signature is invalid.");
        if (sample >= options.warmup) encode.push_back(seconds);
    }
    reporter.report(config.tag, "encode", {1, 0, true}, options, 1, std::move(encode));
    std::vector<double> decode;
    std::optional<Shader> decoded;
    for (size_t sample = 0; sample <= options.warmup + options.repetitions; ++sample) {
        decoded.reset();
        const auto started = Clock::now();
        decoded.emplace(GPU::decode(encoded));
        const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
        if (sample == 0) reporter.report(config.tag, "first-decode-cache-miss", {1, 0, true},
            options, 1, {seconds});
        else if (sample > options.warmup) decode.push_back(seconds);
        Workload check(*decoded, 1, 1, 1, 1, false);
        check.prepare();
        check.run(0);
        check.validate();
    }
    reporter.report(config.tag, "decode-cache-hit", {1, 0, true}, options, 1, std::move(decode));
    return std::move(*shader);
}
/** --------------------------------------------------------------------------------------------------------- Measure
 * @brief Emits raw completed-dispatch samples and separate instrumented admission/completion latencies.
 */
void measure(const Configuration& config, Reporter& reporter, std::ofstream& latency_csv,
    Shader& shader, size_t workers, size_t batch, bool awaiter
) {
    Options options = config.common;
    options.batch = batch;
    options.capacity = config.depth;
    const Shape shape{workers, 0, workers == 1};
    const std::string label = std::string(awaiter ? "awaiter" : "callback") + "-batch-"
        + std::to_string(batch) + "-depth-" + std::to_string(config.depth);
    Progress progress(config.tag + ' ' + label + ' ' + shape.label(), options.timeout);
    Workload workload(shader, options.items, batch, workers, config.depth, awaiter);
    Team team(workload, workers);
    std::vector<double> throughput;
    for (size_t sample = 0; sample < options.warmup + options.repetitions; ++sample) {
        workload.prepare();
        const double elapsed = team.measure();
        workload.validate();
        if (sample >= options.warmup) throughput.push_back(elapsed);
    }
    reporter.report(config.tag, label, shape, options, options.items, std::move(throughput));
    workload.instrumented = true;
    std::vector<double> latencies;
    for (size_t sample = 0; sample < options.repetitions; ++sample) {
        workload.prepare();
        static_cast<void>(team.measure());
        workload.validate();
        for (size_t index = 0; index < workload.dispatches.size(); ++index) {
            const Dispatch& dispatch = *workload.dispatches[index];
            latencies.push_back(dispatch.nanoseconds);
            latency_csv << config.backend << ',' << config.source << ',' << label << ','
                << workers << ',' << batch << ',' << config.depth
                << ',' << sample + 1 << ',' << index << ',' << std::setprecision(12)
                << dispatch.admission_nanoseconds << ',' << dispatch.nanoseconds << '\n';
        }
    }
    latency_csv.flush();
    require(latency_csv.good(), "Failed to write GPU latency samples.");
    std::sort(latencies.begin(), latencies.end());
    LOG_INFO_STREAM << config.tag << ' ' << label << ' ' << shape.label()
        << " host submit-to-completion p50=" << latencies[(latencies.size() - 1) / 2]
        << " ns; p95=" << latencies[(latencies.size() - 1) * 95 / 100]
        << " ns; p99=" << latencies[(latencies.size() - 1) * 99 / 100]
        << " ns; samples=" << latencies.size();
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Measures one selected backend and source path in a fresh process.
 */
int main(int count, char** arguments) {
    try {
        const Configuration config = configuration(count, arguments);
        if (config.common.help) {
            LOG_INFO_STREAM << "Set ALLIGATOR_GPU_BACKEND=vulkan or metal before process startup; "
                << "--source translated (default) uses GLSL; --source native uses Metal MSL only.";
            LOG_INFO_STREAM << "Options: --items N (128 dispatches) --warmup N (2) --repetitions N (15) "
                << "--submitters N --single-thread --depth N (8) --batch N (default 1/16/256) "
                << "--timeout SECONDS (120) --csv PATH (BACKEND-SOURCE_dispatch_samples.csv)";
            return 0;
        }
#ifndef NDEBUG
        throw std::runtime_error("GPU performance runs require a Release build from ./run_build.sh.");
#endif
        require(config.backend == "vulkan" || config.backend == "metal",
            "Select ALLIGATOR_GPU_BACKEND=vulkan or metal before process startup.");
        require(config.source != "native" || config.backend == "metal",
            "--source native requires the Metal backend.");
        Reporter reporter(config.common);
        {
            Progress progress(config.tag + " context initialization", config.common.timeout);
            const auto started = Clock::now();
            require(GPU::exists(), "The GPU benchmark requires a hardware compute device.");
            const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
            reporter.report(config.tag, "context-initialization", {1, 0, true}, config.common, 1, {seconds});
        }
        const GPUDevice& device = gpu_device();
        require(device.kind == (config.backend == "metal" ? GPUBackend::Metal : GPUBackend::Vulkan),
            "The frozen GPU backend differs from the requested measurement.");
        LOG_INFO_STREAM << "backend=" << config.backend << "; source=" << config.source
            << "; device=" << GPU::device_name() << "; unified=" << GPU::unified_memory()
            << "; placement=" << device.placement->type_name
            << "; workgroups per stream=1; stream bytes=64";
        LOG_INFO_STREAM << "Host admission and callback/awaiter observation include scheduler costs; "
            << "device timestamps, bandwidth, and memory high-water are unmeasured.";
        std::ofstream latency_csv(config.common.csv + ".latencies.csv");
        require(latency_csv.is_open(), "Cannot open GPU latency CSV output.");
        latency_csv << "backend,source,workload,submitters,batch,depth,repetition,dispatch_index,admission_nanoseconds,completion_nanoseconds\n";
        Shader shader = preparation(config, reporter);
        std::vector<size_t> workers;
        if (config.common.single_thread) workers.push_back(1);
        else if (config.submitters) workers.push_back(config.submitters);
        else if (device.kind == GPUBackend::Vulkan) {
            workers = {1, VulkanContext::queue_count(), VulkanContext::queue_count() + 2};
            workers.erase(std::unique(workers.begin(), workers.end()), workers.end());
            LOG_INFO_STREAM << "Vulkan submitter candidates derive from compute queue count="
                << VulkanContext::queue_count();
        } else {
            const size_t processors = std::thread::hardware_concurrency();
            require(processors != 0, "CPU count unavailable; supply --submitters N.");
            workers = {1, processors, processors + 2};
            workers.erase(std::unique(workers.begin(), workers.end()), workers.end());
            LOG_INFO_STREAM << "Metal submitter candidates use host logical CPUs=" << processors
                << "; this benchmark topology does not describe GPU queues or prepared slots";
        }
        const std::vector<size_t> batches = config.batch ? std::vector<size_t>{config.batch}
            : std::vector<size_t>{1, 16, 256};
        for (const size_t submitters : workers) {
            for (const size_t batch : batches) {
                measure(config, reporter, latency_csv, shader, submitters, batch, false);
                measure(config, reporter, latency_csv, shader, submitters, batch, true);
            }
        }
        LOG_INFO_STREAM << config.tag << " raw samples: " << config.common.csv << " and "
            << config.common.csv << ".latencies.csv";
        return 0;
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "GPU dispatch benchmark failed: " << error.what();
        return 1;
    }
}
