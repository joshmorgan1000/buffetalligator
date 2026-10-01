/** --------------------------------------------------------------------------------------------------------- Shader Failure Tests
 * @file shader_failure_test.cpp
 * @brief Injects preparation and submission failures into the production Shader implementation.
 */
#define BUFFETALLIGATOR_SHADER_TESTING 1
#include "../src/vulkan/vulkan.cpp"
#include "../src/gpu/shader.cpp"
#include "functional_support.hpp"
#include <atomic>
#include <coroutine>
#include <exception>
#include <span>

using namespace buffetalligator;
using functional::require;
/** --------------------------------------------------------------------------------------------------------- Increment Body
 * @brief Changes one mapped word after an admitted submission reaches the GPU.
 */
static constexpr std::string_view INCREMENT_BODY = R"glsl(
void alligator_main(Slice stream) {
    if (gl_LocalInvocationIndex == 0u)
        slice_store_u32(stream, 0u, slice_load_u32(stream, 0u) + 1u);
}
)glsl";
/** --------------------------------------------------------------------------------------------------------- Preparation Failures
 * @brief Checks partial slot cleanup, invalid stages, compilation rejection, and successful retry.
 */
static void preparation_failures() {
    shader_test_fail_preparation.store(true, std::memory_order_release);
    bool rejected = false;
    try { Shader failed(INCREMENT_BODY, "injected_preparation"); }
    catch (const GPUException&) { rejected = true; }
    require(rejected && !shader_test_fail_preparation.load(),
        "The preparation failure did not run after allocating slot storage");
    require(shader_test_live_slots.load() == 0, "Failed preparation retained a partial submission slot");
    rejected = false;
    Slice references(64, VulkanContext::buffer_placement());
    try { ShaderState failed(std::span<const KernelGpuStage>{}, "empty_stages", references, 0); }
    catch (const GPUException&) { rejected = true; }
    require(rejected, "An empty stage sequence reached the first-stage lookup");
    rejected = false;
    try { Shader failed("invalid GLSL source", "rejected_compilation"); }
    catch (const GPUException&) { rejected = true; }
    require(rejected, "Invalid GLSL did not report its preparation failure");
    {
        Shader recovered(INCREMENT_BODY, "recovered_preparation");
        require(shader_test_live_slots.load() == VulkanContext::queue_count(),
            "Preparation did not recover after unwinding its partial resources");
    }
    require(shader_test_live_slots.load() == 0, "Prepared submission slots survived their owner");
}
/** --------------------------------------------------------------------------------------------------------- Rejected Admission
 * @brief Proves a rejected result releases captured identities and prepared resources before returning.
 */
static void rejected_admission() {
    ShaderResult result;
    auto shader = std::make_unique<Shader>(INCREMENT_BODY, "rejected_admission");
    Slice payload(64, true, VulkanContext::buffer_placement());
    SliceEntry* entry = SliceEntry::from_slice(payload);
    TaskCountdown unexpected;
    shader_test_fail_admission.store(true, std::memory_order_release);
    bool rejected = false;
    try { (*shader)(payload, result, &TaskCountdown::arrive, &unexpected); }
    catch (const std::bad_alloc&) { rejected = true; }
    require(rejected && !result.ready(), "Rejected admission published a completed result");
    require(entry->owners.load(std::memory_order_acquire) == 1,
        "Rejected admission left a captured Slice identity in its result");
    require(unexpected.pending.load(std::memory_order_acquire) == 1,
        "Rejected admission invoked a completion callback");
    shader.reset();
    require(shader_test_live_slots.load() == 0,
        "Rejected admission kept its destroyed Shader alive through the caller's result");
    payload.free();
    require(entry->owners.load(std::memory_order_acquire) == 0,
        "Rejected admission prevented the caller's final release");
}
/** --------------------------------------------------------------------------------------------------------- Completion Observation
 * @brief Checks the owned result and retained payload while a callback owns completion.
 */
struct CompletionObservation {
    ShaderResult result;
    TaskCountdown complete;
    std::atomic<uint32_t> callbacks{0};
    SliceEntry* entry = nullptr;
    uint32_t* payload = nullptr;
    uint32_t expected_value = 0;
    uint32_t expected_owners = 0;
    bool expected_failure = false;
    static void done(void* pointer) {
        auto& state = *static_cast<CompletionObservation*>(pointer);
        require(state.result.ready(), "Completion ran before terminal state was published");
        bool failed = false;
        try { state.result.rethrow(); } catch (const GPUException&) { failed = true; }
        require(failed == state.expected_failure, "Completion observed an incorrect terminal result");
        require(state.entry->owners.load(std::memory_order_acquire) == state.expected_owners
            && *state.payload == state.expected_value,
            "Completion lost its retained identity or observed an unsubmitted write");
        state.callbacks.fetch_add(1, std::memory_order_relaxed);
        TaskCountdown::arrive(&state.complete);
    }
};
/** --------------------------------------------------------------------------------------------------------- Submission Failure
 * @brief Verifies exactly-once failed completion, retained ownership, and subsequent slot reuse.
 */
static void submission_failure() {
    Shader shader(INCREMENT_BODY, "submission_failure");
    Slice payload(64, true, VulkanContext::buffer_placement());
    payload.get_as<uint32_t>() = 41;
    CompletionObservation failure;
    failure.entry = SliceEntry::from_slice(payload);
    failure.payload = payload.data<uint32_t>();
    failure.expected_value = 41;
    failure.expected_owners = 1;
    failure.expected_failure = true;
    shader_test_hold_submission.store(true, std::memory_order_release);
    shader_test_fail_submission.store(true, std::memory_order_release);
    shader(payload, failure.result, &CompletionObservation::done, &failure);
    payload.free();
    shader_test_hold_submission.store(false, std::memory_order_release);
    shader_test_hold_submission.notify_all();
    failure.complete.wait();
    bool failed = false;
    try { failure.result.rethrow(); } catch (const GPUException&) { failed = true; }
    require(failed && failure.callbacks.load() == 1, "Submission failure was lost or completed twice");
    require(failure.entry->owners.load(std::memory_order_acquire) == 0,
        "Failed completion retained the released caller's Slice identity");
    Slice next(64, VulkanContext::buffer_placement());
    next.get_as<uint32_t>() = 91;
    CompletionObservation success;
    success.entry = SliceEntry::from_slice(next);
    success.payload = next.data<uint32_t>();
    success.expected_value = 92;
    success.expected_owners = 2;
    shader(next, success.result, &CompletionObservation::done, &success);
    success.complete.wait();
    success.result.rethrow();
    require(success.callbacks.load() == 1 && next.get_as<uint32_t>() == 92,
        "A failed submission poisoned its prepared slot");
}
/** --------------------------------------------------------------------------------------------------------- Coroutine Observation
 * @brief Holds coroutine completion evidence outside the suspended frame.
 */
struct CoroutineObservation {
    TaskCountdown complete;
    std::atomic<uint32_t> failures{0};
};
/** --------------------------------------------------------------------------------------------------------- Failure Task
 * @brief Cancels through retained operation state before destroying a managed coroutine frame.
 */
struct FailureTask {
    struct promise_type {
        FailureTask get_return_object() {
            return FailureTask(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };
    std::coroutine_handle<promise_type> frame;
    ShaderResult cancellation;
    explicit FailureTask(std::coroutine_handle<promise_type> handle) : frame(handle) {}
    FailureTask(const FailureTask&) = delete;
    ~FailureTask() { cancellation.cancel(); frame.destroy(); }
};
/** --------------------------------------------------------------------------------------------------------- Await Failure
 * @brief Observes the accepted operation's terminal failure at the coroutine resume point.
 */
static FailureTask await_failure(ShaderAwaiter awaiter, CoroutineObservation* context) {
    try { co_await std::move(awaiter); }
    catch (const GPUException&) { context->failures.fetch_add(1, std::memory_order_relaxed); }
    TaskCountdown::arrive(&context->complete);
}
/** --------------------------------------------------------------------------------------------------------- Coroutine Failure
 * @brief Delivers a failed submission to one resumed coroutine after public Shader destruction.
 */
static void coroutine_failure() {
    auto shader = std::make_unique<Shader>(INCREMENT_BODY, "coroutine_failure");
    Slice payload(64, VulkanContext::buffer_placement());
    shader_test_hold_submission.store(true, std::memory_order_release);
    shader_test_fail_submission.store(true, std::memory_order_release);
    auto awaiter = shader->dispatch(&payload, 1);
    ShaderResult cancellation = awaiter.result();
    CoroutineObservation observation;
    FailureTask task = await_failure(std::move(awaiter), &observation);
    task.cancellation = cancellation;
    task.frame.resume();
    shader.reset();
    payload.free();
    shader_test_hold_submission.store(false, std::memory_order_release);
    shader_test_hold_submission.notify_all();
    observation.complete.wait();
    cancellation.cancel();
    require(observation.failures.load() == 1, "Coroutine completion did not report its terminal failure");
    require(shader_test_live_slots.load() == 0,
        "Completed coroutine work retained its destroyed Shader's slots");
}
/** --------------------------------------------------------------------------------------------------------- Cache Retry
 * @brief Retries the same portable program after a failed first preparation without a poisoned entry.
 */
static void cache_retry() {
    Shader source(INCREMENT_BODY, "cache_retry");
    Slice encoded = GPU::encode(source);
    const uint32_t initial_slots = shader_test_live_slots.load();
    shader_test_fail_preparation.store(true, std::memory_order_release);
    bool rejected = false;
    try { Shader failed = GPU::decode(encoded); } catch (const GPUException&) { rejected = true; }
    require(rejected && shader_test_live_slots.load() == initial_slots,
        "Failed cached preparation left partial prepared resources");
    Shader recovered = GPU::decode(encoded);
    Slice payload(64, VulkanContext::buffer_placement());
    payload.get_as<uint32_t>() = 123;
    ShaderResult result;
    TaskCountdown complete;
    recovered(payload, result, &TaskCountdown::arrive, &complete);
    complete.wait();
    result.rethrow();
    require(payload.get_as<uint32_t>() == 124, "A failed cache insertion prevented later preparation");
}
/** --------------------------------------------------------------------------------------------------------- Shutdown Admission
 * @brief Holds capture before acceptance while final shutdown closes and waits for its ownership.
 */
struct ShutdownAdmission {
    Shader shader{INCREMENT_BODY, "shutdown_admission"};
    Slice payload{64, VulkanContext::buffer_placement()};
    ShaderResult result;
    std::atomic<bool> rejected{false};
    static void submit(ShutdownAdmission* pending) {
        try { pending->shader(pending->payload, pending->result); }
        catch (const GPUException&) { pending->rejected.store(true, std::memory_order_release); }
    }
};
/** --------------------------------------------------------------------------------------------------------- Shutdown Capture
 * @brief Prevents arena shutdown from passing an unaccepted operation that already retained Slice ids.
 */
static void shutdown_capture() {
    Kitchen::inst().drain();
    ShutdownAdmission pending;
    shader_test_admission_entered.store(false, std::memory_order_release);
    shader_test_hold_admission.store(true, std::memory_order_release);
    std::thread producer(&ShutdownAdmission::submit, &pending);
    shader_test_admission_entered.wait(false, std::memory_order_acquire);
    std::thread shutdown(&ShaderState::drain);
    for (;;) {
        bool closed;
        {
            std::lock_guard lock(shader_runtime().mutex);
            closed = shader_runtime().stopping;
            if (closed) require(shader_runtime().active == 1,
                "Shutdown did not account for capture blocked before acceptance");
        }
        if (closed) break;
        std::this_thread::yield();
    }
    require(SliceEntry::from_slice(pending.payload)->owners.load(std::memory_order_acquire) == 2,
        "The held admission did not retain its captured Slice identity");
    shader_test_hold_admission.store(false, std::memory_order_release);
    shader_test_hold_admission.notify_all();
    producer.join();
    shutdown.join();
    require(pending.rejected.load(std::memory_order_acquire) && !pending.result.ready(),
        "An operation crossed final shutdown's closed admission gate");
    require(SliceEntry::from_slice(pending.payload)->owners.load(std::memory_order_acquire) == 1,
        "Shutdown rejection retained the captured Slice after its producer returned");
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs hardware-backed failure paths without submitting any failing command to the device.
 */
int main() {
    if (!VulkanKernel::available()) {
        LOG_INFO_STREAM << "Skipping Shader failure tests: no Vulkan compute device";
        return 77;
    }
    try {
        preparation_failures();
        rejected_admission();
        submission_failure();
        coroutine_failure();
        cache_retry();
        shutdown_capture();
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Shader failure test failed: " << error.what();
        return 1;
    }
    LOG_INFO_STREAM << "Shader preparation, submission, coroutine, and cache failure paths passed";
    return 0;
}
