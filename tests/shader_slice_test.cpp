/** --------------------------------------------------------------------------------------------------------- Shader Slice Tests
 * @file shader_slice_test.cpp
 * @brief Exercises granule lookup, retained asynchronous dispatch, and portable shader preparation.
 */
#include <alligator.hpp>
#include <alligator/easygpu.hpp>
#include <alligator/easyvulkan.hpp>
#include <alligator/kitchen.hpp>
#include <vulkan/shader_state.hpp>
#include "functional_support.hpp"
#include <array>
#include <atomic>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <thread>
#include <vector>

using namespace buffetalligator;
using functional::require;
/** --------------------------------------------------------------------------------------------------------- Identity Body
 * @brief Reports retained identities, byte addresses, and granule lengths for two embedded inputs.
 */
static constexpr std::string_view IDENTITY_BODY = R"glsl(
void alligator_main(Slice stream) {
    if (gl_LocalInvocationIndex != 0u) return;
    Slice first = gpu_slice(slice_load_u32(stream, 0u));
    Slice second = gpu_slice(slice_load_u32(stream, 1u));
    slice_store_u64(stream, 2u, slice_address(first));
    slice_store_u64(stream, 3u, slice_address(second));
    slice_store_u64(stream, 4u, slice_size(first));
    slice_store_u64(stream, 5u, gpu_slice_size(second.id));
    slice_store_u32x4(stream, 3u, uvec4(vulkan_count(), vulkan_index(), stream.id, 77u));
}
)glsl";
/** --------------------------------------------------------------------------------------------------------- Direct Dispatch
 * @brief Resolves rounded views through their region table without changing the slab base.
 */
static void direct_dispatch() {
    Slice storage(512, VulkanContext::buffer_placement());
    std::array<Slice, 2> payloads{storage.slice(5, 20), storage.slice(130, 50)};
    std::array<Slice, 2> streams{Slice(64, VulkanContext::buffer_placement()),
        Slice(64, VulkanContext::buffer_placement())};
    for (Slice& stream : streams) {
        stream.data<uint32_t>()[0] = payloads[0].id();
        stream.data<uint32_t>()[1] = payloads[1].id();
    }
    const GPUBuf* record = Alligator::gpubuf_for(payloads[1]);
    require(record == Alligator::gpu_table(uint8_t((payloads[1].id() >> 3) & 63)) + (payloads[1].id() >> 9),
        "Slice identity resolved the wrong region entry");
    require(record->offset == Alligator::gpubuf_for(storage)->offset + 2 && record->size == 1,
        "Rounded view metadata is not expressed in granules");
    ShaderState shader(IDENTITY_BODY, "slice_identity");
    shader.dispatch(streams.data(), streams.size(), 1);
    for (size_t index = 0; index < streams.size(); ++index) {
        const auto* addresses = streams[index].data<uint64_t>();
        const auto* words = streams[index].data<uint32_t>();
        require(addresses[2] == VulkanKernel::device_address(storage)
            && addresses[3] == VulkanKernel::device_address(storage) + 128,
            "Shader lookup applied the wrong granule offset");
        require(addresses[4] == 64 && addresses[5] == 64, "Shader sizes did not widen granules to bytes");
        require(words[12] == 2 && words[13] == index && words[14] == streams[index].id(),
            "Dispatch did not preserve its parameter identities and live count");
    }
}
/** --------------------------------------------------------------------------------------------------------- Region Spread
 * @brief Resolves retained embedded identities across independently allocated GPU metadata regions.
 */
static void region_spread() {
    Slice storage(256, true, VulkanContext::buffer_placement());
    std::array<Slice, 2> dependencies{storage.slice(0, 64), Slice{}};
    const uint32_t first_region = (dependencies[0].id() >> 3) & 63;
    std::vector<Slice> aliases;
    aliases.reserve(REGION_SIZE + 1);
    for (size_t index = 0; index <= REGION_SIZE; ++index) {
        aliases.push_back(storage.slice(128, 128));
        if (((aliases.back().id() >> 3) & 63) != first_region) {
            dependencies[1] = std::move(aliases.back());
            aliases.pop_back();
            break;
        }
    }
    require(!dependencies[1].is_null(), "Live Slice aliases did not cross a metadata region");
    const std::array<uint64_t, 2> addresses{VulkanKernel::device_address(dependencies[0]),
        VulkanKernel::device_address(dependencies[1])};
    Slice invocation(64, VulkanContext::buffer_placement());
    invocation.data<uint32_t>()[0] = dependencies[0].id();
    invocation.data<uint32_t>()[1] = dependencies[1].id();
    Shader shader(IDENTITY_BODY, "region_spread");
    ShaderResult result;
    TaskCountdown complete;
    shader(invocation, result, &TaskCountdown::arrive, &complete, 1, dependencies);
    dependencies[0].free();
    dependencies[1].free();
    storage.free();
    aliases.clear();
    complete.wait();
    result.rethrow();
    const uint64_t* actual = invocation.data<uint64_t>();
    require(actual[2] == addresses[0] && actual[3] == addresses[1],
        "GPU lookup did not use both entries in the metadata region directory");
    require(actual[4] == 64 && actual[5] == 128,
        "Cross-region dependencies lost their independent granule bounds");
}
/** --------------------------------------------------------------------------------------------------------- Reference Dispatch
 * @brief Resolves a prepared reference ring using granule-aligned invocation records.
 */
static void reference_dispatch() {
    Slice source(64, VulkanContext::buffer_placement());
    Slice destination(64, VulkanContext::buffer_placement());
    Slice invocation(64, VulkanContext::buffer_placement());
    Slice references(64, VulkanContext::buffer_placement());
    source.get_as<uint32_t>() = 91;
    invocation.data<uint32_t>()[0] = source.id();
    invocation.data<uint32_t>()[1] = destination.id();
    references.data<uint32_t>()[3] = invocation.id();
    ShaderState shader(R"glsl(
void alligator_main(Slice source, Slice destination) {
    if (gl_LocalInvocationIndex != 0u) return;
    slice_store_u32(destination, 0u, slice_load_u32(source, 0u));
    slice_store_u64(destination, 1u, slice_size(source));
}
)glsl", "reference_identity", &references);
    shader.dispatch_references(3, 1);
    require(destination.get_as<uint32_t>() == 91 && destination.data<uint64_t>()[1] == 64,
        "The reference ring resolved an incorrect invocation or size");
}
/** --------------------------------------------------------------------------------------------------------- Job Dispatch
 * @brief Runs the encoded public job-table example across two independent mapped inputs.
 */
static void job_dispatch() {
    std::array<Slice, 2> streams{Slice(64, VulkanContext::buffer_placement()),
        Slice(64, VulkanContext::buffer_placement())};
    for (Slice& stream : streams) {
        stream.data<uint32_t>()[1] = 4;
        const std::array<float, 8> values{1, 2, 3, 4, 2, 4, 6, 8};
        stream.get_as<std::array<float, 16>>()[4] = values[0];
        stream.data<float>()[5] = values[1]; stream.data<float>()[6] = values[2];
        stream.data<float>()[7] = values[3]; stream.data<float>()[8] = values[4];
        stream.data<float>()[9] = values[5]; stream.data<float>()[10] = values[6];
        stream.data<float>()[11] = values[7];
    }
    Slice encoded = GPU::compile_glsl(VULKAN_GLSL_L2_EXAMPLE);
    TaskCountdown complete;
    ShaderResult result;
    GPU::run(encoded, streams.data(), streams.size(), result, &TaskCountdown::arrive, &complete);
    encoded.free();
    complete.wait();
    result.rethrow();
    require(streams[0].get_as<float>() == 30 && streams[1].get_as<float>() == 30,
        "Encoded job dispatch failed after program ownership was released");
}
/** --------------------------------------------------------------------------------------------------------- Embedded Payload
 * @brief Stores actual owning Slice objects at the four-byte GLSL identity stride.
 */
struct EmbeddedPayload {
    std::array<Slice, 2> payloads;
    uint32_t marker;
};
static_assert(offsetof(EmbeddedPayload, marker) == 8);
/** --------------------------------------------------------------------------------------------------------- Embedded Handles
 * @brief Retains registered embedded identities after public owners are released.
 */
static void embedded_handles() {
    Slice storage(64, VulkanContext::buffer_placement());
    EmbeddedPayload* embedded = std::construct_at(storage.data<EmbeddedPayload>(), EmbeddedPayload{
        {Slice(64, true, VulkanContext::buffer_placement()), Slice(64, true, VulkanContext::buffer_placement())}, 123});
    embedded->payloads[0].get_as<uint32_t>() = 17;
    Shader shader(R"glsl(
void alligator_main(Slice invocation) {
    if (gl_LocalInvocationIndex != 0u) return;
    Slice source = gpu_slice(slice_load_u32(invocation, 0u));
    Slice destination = gpu_slice(slice_load_u32(invocation, 1u));
    slice_store_u32(destination, 0u, slice_load_u32(source, 0u) + 1u);
}
)glsl", "embedded_handles");
    TaskCountdown complete;
    ShaderResult result;
    shader(storage, result, &TaskCountdown::arrive, &complete, 1, embedded->payloads);
    complete.wait();
    result.rethrow();
    require(embedded->payloads[1].get_as<uint32_t>() == 18 && embedded->marker == 123,
        "Registered embedded Slice identities did not survive dispatch");
    std::destroy_at(embedded);
}
/** --------------------------------------------------------------------------------------------------------- Dispatch Rounds
 * @brief Checks exact live counts across full and partial rounds without mutating shared records.
 */
static void dispatch_rounds() {
    ShaderState shader(R"glsl(
void alligator_main(Slice stream) {
    if (gl_LocalInvocationIndex != 0u) return;
    slice_store_u32x4(stream, 0u, uvec4(stream.id, vulkan_count(), vulkan_index(), 29u));
}
)glsl", "dispatch_rounds");
    const size_t count = shader.capacity() + 3;
    Slice backing(count * 64, VulkanContext::buffer_placement());
    std::vector<Slice> streams;
    streams.reserve(count);
    for (size_t index = 0; index < count; ++index) streams.push_back(backing.slice(index * 64, 64));
    shader.dispatch(streams.data(), streams.size(), 1);
    for (size_t index = 0; index < count; ++index) {
        const auto* words = streams[index].data<uint32_t>();
        require(words[0] == streams[index].id()
            && words[1] == (index < shader.capacity() ? shader.capacity() : 3)
            && words[2] == index % shader.capacity(), "A partial round retained stale parameter metadata");
    }
    shader.dispatch(streams.data() + shader.capacity(), 1, 1);
    require(streams[shader.capacity()].data<uint32_t>()[1] == 1,
        "A short dispatch did not replace its previous indirect dimensions");
}
/** --------------------------------------------------------------------------------------------------------- Increment Body
 * @brief Adds one to the first word of each retained stream.
 */
static constexpr std::string_view INCREMENT_BODY = R"glsl(
void alligator_main(Slice stream) {
    if (gl_LocalInvocationIndex == 0u) slice_store_u32(stream, 0u, slice_load_u32(stream, 0u) + 1u);
}
)glsl";
/** --------------------------------------------------------------------------------------------------------- Reentrant Completion
 * @brief Owns completion contexts that may submit again and destroy their Shader from a callback.
 */
struct ReentrantCompletion {
    TaskCountdown complete{2};
    ShaderResult first;
    ShaderResult second;
    std::unique_ptr<Shader> shader;
    Slice next;
    uint32_t* original = nullptr;
    std::atomic<uint32_t> callbacks{0};
    static void finish(void* context) {
        auto& state = *static_cast<ReentrantCompletion*>(context);
        state.second.rethrow();
        require(state.next.get_as<uint32_t>() == 2, "Nested dispatch lost its source");
        state.callbacks.fetch_add(1);
        TaskCountdown::arrive(&state.complete);
    }
    static void reenter(void* context) {
        auto& state = *static_cast<ReentrantCompletion*>(context);
        state.first.rethrow();
        require(*state.original == 42, "Callback lost a released directly bound Slice");
        (*state.shader)(state.next, state.second, &finish, &state);
        state.shader.reset();
        state.callbacks.fetch_add(1);
        TaskCountdown::arrive(&state.complete);
    }
};
/** --------------------------------------------------------------------------------------------------------- Concurrent Submission
 * @brief Binds independent outputs from more submitting threads than available device queues.
 */
struct ConcurrentSubmission {
    Shader* shader;
    Slice stream;
    ShaderResult result;
    TaskCountdown* complete;
    static void run(ConcurrentSubmission* context) {
        (*context->shader)(context->stream, context->result, &TaskCountdown::arrive, context->complete);
    }
};
/** --------------------------------------------------------------------------------------------------------- Throwing Completion
 * @brief Signals observation before exercising terminal callback exception capture.
 */
static void throwing_completion(void* context) {
    TaskCountdown::arrive(context);
    ALLIGATOR_GPU_THROW("Expected completion callback failure");
}
/** --------------------------------------------------------------------------------------------------------- Async Completion
 * @brief Verifies retained resources, callback reentry, destruction, zero work, and owned failures.
 */
static void async_completion() {
    ReentrantCompletion state;
    state.shader = std::make_unique<Shader>(INCREMENT_BODY, "async_completion");
    state.next = Slice(64, VulkanContext::buffer_placement());
    state.next.get_as<uint32_t>() = 1;
    Slice first(64, true, VulkanContext::buffer_placement());
    first.get_as<uint32_t>() = 41;
    state.original = first.data<uint32_t>();
    (*state.shader)(first, state.first, &ReentrantCompletion::reenter, &state);
    first.free();
    state.complete.wait();
    state.second.rethrow();
    require(state.callbacks.load() == 2 && !state.shader, "Callbacks were duplicated or destruction blocked");
    Shader zero(INCREMENT_BODY, "zero_dispatch");
    ShaderResult result;
    TaskCountdown complete;
    zero(nullptr, 0, result, &TaskCountdown::arrive, &complete);
    complete.wait();
    result.rethrow();
    Slice heap(64, BuffetDescriptors::get(0));
    bool rejected = false;
    try { zero(heap, result); } catch (const GPUException&) { rejected = true; }
    require(rejected, "A non-device Slice was accepted by GPU admission");
    complete.rearm(1);
    zero(nullptr, 0, result, &throwing_completion, &complete);
    complete.wait();
    bool callback_failed = false;
    try { result.rethrow(); } catch (const GPUException&) { callback_failed = true; }
    require(callback_failed, "A callback exception was not retained in its result");
    const size_t producers = VulkanContext::queue_count() + 2;
    TaskCountdown concurrent_complete{uint32_t(producers)};
    std::vector<std::unique_ptr<ConcurrentSubmission>> submissions;
    std::vector<std::thread> threads;
    for (size_t index = 0; index < producers; ++index) {
        auto submission = std::make_unique<ConcurrentSubmission>();
        submission->shader = &zero;
        submission->stream = Slice(64, VulkanContext::buffer_placement());
        submission->stream.get_as<uint32_t>() = uint32_t(index);
        submission->complete = &concurrent_complete;
        threads.emplace_back(&ConcurrentSubmission::run, submission.get());
        submissions.push_back(std::move(submission));
    }
    for (std::thread& thread : threads) thread.join();
    concurrent_complete.wait();
    for (size_t index = 0; index < producers; ++index) {
        submissions[index]->result.rethrow();
        require(submissions[index]->stream.get_as<uint32_t>() == index + 1,
            "Concurrent admitted operations shared mutable submission resources");
    }
}
/** --------------------------------------------------------------------------------------------------------- Program Roundtrip
 * @brief Checks source ownership, portable serialization, cache identity, and malformed input rejection.
 */
static void program_roundtrip() {
    std::string source(INCREMENT_BODY);
    Shader original(ShaderSource{source, "metal body", "cuda body"}, "portable_program");
    source.clear();
    Slice encoded = GPU::encode(original);
    const uint8_t* header = encoded.data<uint8_t>();
    require(header[0] == 'B' && header[1] == 'A' && header[2] == 'G' && header[3] == 'P',
        "Portable encoding does not use the BAGP signature");
    Shader decoded = GPU::decode(encoded);
    Slice payload(64, VulkanContext::buffer_placement());
    payload.get_as<uint32_t>() = 9;
    ShaderResult result;
    TaskCountdown complete;
    decoded(payload, result, &TaskCountdown::arrive, &complete);
    complete.wait();
    result.rethrow();
    require(payload.get_as<uint32_t>() == 10, "Decoded source did not reproduce the prepared program");
    encoded.data<uint8_t>()[24] ^= 1;
    bool rejected = false;
    try { Shader invalid = GPU::decode(encoded); } catch (const GPUException&) { rejected = true; }
    require(rejected, "Corrupt encoded source lengths were accepted");
}
/** --------------------------------------------------------------------------------------------------------- Coroutine Context
 * @brief Records a managed coroutine's resumed state outside its frame.
 */
struct CoroutineContext {
    TaskCountdown complete;
    std::atomic<bool> resumed{false};
};
/** --------------------------------------------------------------------------------------------------------- Coroutine Task
 * @brief Cancels through an externally owned operation handle before destroying its frame.
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
    ShaderResult cancellation;
    explicit CoroutineTask(std::coroutine_handle<promise_type> handle) : frame(handle) {}
    CoroutineTask(const CoroutineTask&) = delete;
    ~CoroutineTask() { cancellation.cancel(); frame.destroy(); }
};
/** --------------------------------------------------------------------------------------------------------- Await Dispatch
 * @brief Suspends only while the retained operation is pending.
 */
static CoroutineTask await_dispatch(ShaderAwaiter awaiter, CoroutineContext* context) {
    co_await std::move(awaiter);
    context->resumed.store(true, std::memory_order_release);
    TaskCountdown::arrive(&context->complete);
}
/** --------------------------------------------------------------------------------------------------------- Coroutine Completion
 * @brief Exercises completion-publication races and cancellation before managed frame destruction.
 */
static void coroutine_completion() {
    Shader shader(INCREMENT_BODY, "coroutine_completion");
    Slice payload(64, VulkanContext::buffer_placement());
    CoroutineContext context;
    auto awaiter = shader.dispatch(&payload, 1);
    ShaderResult cancellation = awaiter.result();
    CoroutineTask task = await_dispatch(std::move(awaiter), &context);
    task.cancellation = cancellation;
    task.frame.resume();
    context.complete.wait();
    require(context.resumed.load(std::memory_order_acquire), "GPU completion did not resume its coroutine");
    for (size_t iteration = 0; iteration < 32; ++iteration) {
        CoroutineContext cancelled_context;
        auto pending = shader.dispatch(nullptr, 0);
        ShaderResult owner = pending.result();
        CoroutineTask cancelled = await_dispatch(std::move(pending), &cancelled_context);
        cancelled.cancellation = owner;
        cancelled.frame.resume();
        owner.cancel();
    }
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs one isolated hardware-backed Shader contract case.
 */
int main(int count, char** arguments) {
    if (!VulkanKernel::available()) {
        LOG_INFO_STREAM << "Skipping shader Slice tests: no Vulkan compute device";
        return 77;
    }
    return functional::run(count, arguments, {{"vulkan_shader_identity", &direct_dispatch},
        {"vulkan_region_spread", &region_spread},
        {"vulkan_reference_dispatch", &reference_dispatch}, {"vulkan_job_dispatch", &job_dispatch},
        {"vulkan_embedded_slices", &embedded_handles}, {"vulkan_dispatch_rounds", &dispatch_rounds},
        {"vulkan_async_completion", &async_completion}, {"vulkan_program_roundtrip", &program_roundtrip},
        {"vulkan_coroutine_completion", &coroutine_completion}});
}
