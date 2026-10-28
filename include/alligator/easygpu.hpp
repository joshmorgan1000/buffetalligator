#pragma once
/** --------------------------------------------------------------------------------------------------------- EasyGPU
 * @file easygpu.hpp
 * @brief Portable prepared shaders with owned completion results and coroutine suspension.
 */
#include <logging.hpp>
#include <alligator.hpp>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <coroutine>
#include <memory>
#include <optional>
#include <semaphore>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace buffetalligator {
EXCEPTION_CLASS(GPU)
#define ALLIGATOR_GPU_THROW(msg) throw GPUException(msg)
/** --------------------------------------------------------------------------------------------------------- GPU Backend
 * @enum GPUBackend
 * @brief The compute domain a process freezes on at its first Slice or GPU operation.
 */
enum class GPUBackend { CPU, Vulkan, Metal, CUDA };
/** --------------------------------------------------------------------------------------------------------- Device Memory Usage
 * @struct DeviceMemoryUsage
 * @brief Reports native device measurements with unavailable quantities left empty.
 */
struct DeviceMemoryUsage {
    uint64_t capacity_bytes;
    uint64_t available_bytes;
};
/** --------------------------------------------------------------------------------------------------------- GPU Capabilities
 * @struct GPUCapabilities
 * @brief The frozen device's probed facts, plain data so callers never see a backend type.
 */
struct GPUCapabilities {
    bool present = false;                        ///< A compute device exists.
    bool unified = false;                        ///< Host and device share physical memory.
    bool supports_float16 = false;               ///< 16-bit float arithmetic in shaders.
    bool supports_int64 = false;                 ///< 64-bit integers in shaders.
    bool supports_memory_budget = false;         ///< Live device-memory headroom queryable.
    bool supports_timeline_semaphore = false;    ///< Timeline semaphores available.
    bool internally_synchronized_queues = false; ///< Queue submission needs no external sync.
    uint32_t queue_count = 0;                    ///< Compute queues across the compute families.
    uint32_t subgroup_size = 0;                  ///< SIMD subgroup width.
    uint32_t max_workgroup_invocations = 0;      ///< Max invocations per workgroup.
    uint64_t device_local_bytes = 0;             ///< Total device-local heap size.
    uint64_t host_visible_bytes = 0;             ///< Heap bytes reachable from the host.
    uint64_t device_local_headroom_bytes = 0;    ///< Live budget headroom at poll time.
    std::string device_name;                     ///< Vendor device name.
};
/** --------------------------------------------------------------------------------------------------------- Shader Source
 * @struct ShaderSource
 * @brief Supplies owned-at-preparation native bodies and the portable GLSL body.
 */
struct ShaderSource {
    std::string_view glsl;
    std::string_view metal;
    std::string_view cuda;
};
/** --------------------------------------------------------------------------------------------------------- Shader Result
 * @class ShaderResult
 * @brief Works much like a future, except it carries a caller-specified context, and uses context-native
 * synchronization primitives rather than standard C++ futures.
 */
class ShaderResult {
private:
    /** ------------------------------------------------------------------------------------------- State
     * @brief What the result and its dispatch share: the callback, its context, and the semaphore
     * the dispatch's retirement releases.
     */
    struct State {
        void (*callback)(std::shared_ptr<void>) = nullptr;  ///< Callback invoked upon shader completion.
        std::shared_ptr<void> context;                       ///< Context associated with the shader result.
        std::binary_semaphore finished{0};                   ///< Released once the dispatch has retired.
    };
    /// @brief The shared state; the dispatch slot holds the other reference until it retires.
    std::shared_ptr<State> state_;
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs a ShaderResult with the specified callback and context. Private because
     * it should only be created by the Shader class immediately before execution.
     * @param callback Function to be called upon shader completion.
     * @param context Context associated with the shader result.
     */
    ShaderResult(void (*callback)(std::shared_ptr<void>), std::shared_ptr<void> context)
    : state_(std::make_shared<State>()) {
        state_->callback = callback;
        state_->context = std::move(context);
    }
    /** ------------------------------------------------------------------------------------------- Complete
     * @brief Invokes the callback with the stored context and releases the semaphore.
     * @param state The dispatch's shared state.
     */
    static void complete(State& state) {
        if (state.callback) {
            state.callback(state.context);
        }
        state.finished.release();
    }
    /// @brief Grants the Shader access to create ShaderResult instances.
    friend class Shader;
public:
    /** ------------------------------------------------------------------------------------------- Move only semantics */
    ShaderResult(const ShaderResult&) = delete;
    ShaderResult& operator=(const ShaderResult&) = delete;
    ShaderResult(ShaderResult&&) noexcept = default;
    ShaderResult& operator=(ShaderResult&&) noexcept = default;
    /** ------------------------------------------------------------------------------------------- Destructor */
    ~ShaderResult() = default;
    /** ------------------------------------------------------------------------------------------- Get
     * @brief Waits for the context-native semaphore to signal completion and then returns the
     * result stored in the context.
     */
    std::shared_ptr<void> get() const {
        state_->finished.acquire();
        state_->finished.release();
        return state_->context;
    }
};
/** --------------------------------------------------------------------------------------------------------- Shader
 * @class Shader
 * @brief A compute shader prepared once: compiled, its pipeline and SLOTS command slots built, a thread per slot
 * waiting on the dispatch queue. A dispatch is one enqueue; the slot's thread records, submits, waits and
 * completes the ShaderResult.
 */
class Shader {
public:
    static constexpr uint32_t SLOTS = 2;  ///< Command slots and the threads that own them, double buffered.
private:
    /** ------------------------------------------------------------------------------------------- Dispatch
     * @struct Dispatch
     * @brief One queued dispatch: the list address, the group counts, its result and the one-id list a
     * single-Slice dispatch reads.
     */
    struct Dispatch {
        uint64_t address = 0;                          ///< The device address of the Slice id list.
        uint32_t groups_x = 0;                         ///< Workgroups along X.
        uint32_t groups_y = 1;                         ///< Workgroups along Y, one per list element.
        std::shared_ptr<ShaderResult::State> result;   ///< Completed on the slot's thread; null stops the thread.
        Slice single;                                  ///< The one-id list a single-Slice dispatch reads.
        bool held = false;                             ///< Whether `single` holds a row id to release.
    };
    vk::PipelineLayout layout_{};
    vk::Pipeline pipeline_{};
    std::array<vk::CommandPool, SLOTS> pools_{};               ///< One pool per slot, recorded from by its thread.
    std::array<vk::CommandBuffer, SLOTS> commands_{};
    std::array<vk::Semaphore, SLOTS> timelines_{};             ///< One timeline per slot, signaled by its submits.
    std::array<uint64_t, SLOTS> tickets_{};                    ///< Each slot's last signal value.
    std::array<std::thread, SLOTS> threads_{};                 ///< One thread per slot, taking turns on the queue.
    mutable moodycamel::BlockingConcurrentQueue<Dispatch> dispatches_;  ///< Dispatches waiting for a slot.
    std::atomic<uint32_t> workgroups_{1};                      ///< Workgroups along X per dispatch.
    uint32_t queue_ = 0;                                       ///< This shader's compute queue.
    /// @brief Compiles the body under the Slice ABI and builds the pipeline, the slots and their threads.
    void prepare(std::string_view glsl, std::string_view name, uint32_t workgroups);
    /// @brief A slot's thread: the next dispatch recorded, submitted, waited and completed; a null result stops it.
    void run(uint32_t slot);
    /// @brief Records the slot for one dispatch: bind, the pushed address, the dispatch and the readback barrier.
    void record(uint32_t slot, uint64_t address, uint32_t groups_x, uint32_t groups_y);
    /// @brief Submits the slot as recorded, signaling its next ticket when it retires.
    void submit(uint32_t slot);
    /// @brief Blocks until the slot's last submit has retired.
    void wait(uint32_t slot) const;
    friend struct GPU;
public:
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Prepares a shader from the specified source.
     * @param source Shader source containing backend-specific bodies.
     * @param name Optional name for the shader.
     * @param workgroups The number of workgroups to configure. Setting to 0 will detect the
     * device's optimal workgroup and use that as the configuration.
     */
    explicit Shader(
        const ShaderSource& source,
        std::string_view name = "alligator_shader",
        uint32_t workgroups = 0
    );
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Prepares a shader from the specified GLSL source.
     * @param glsl GLSL source code for the shader.
     * @param name Optional name for the shader.
     * @param workgroups The number of workgroups to configure. Setting to 0 will detect the
     * device's optimal workgroup and use that as the configuration.
     */
    explicit Shader(
        std::string_view glsl,
        std::string_view name = "alligator_shader",
        uint32_t workgroups = 0
    );
    /** ------------------------------------------------------------------------------------------- Workgroups
     * @brief Retrieves the number of workgroups configured for this shader.
     * @return The number of workgroups.
     */
    uint32_t workgroups() const { return workgroups_.load(std::memory_order_relaxed); }
    /** ------------------------------------------------------------------------------------------- Set Workgroups
     * @brief Sets the number of workgroups for this shader.
     * @param count The number of workgroups to configure.
     */
    void set_workgroups(uint32_t count) { workgroups_.store(count, std::memory_order_relaxed); }
    /** ------------------------------------------------------------------------------------------- No Copy / No Move
     * @brief The slot threads hold this shader's address.
     */
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&&) = delete;
    Shader& operator=(Shader&&) = delete;
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Stops each slot's thread after its last dispatch retires, then releases the slots and the pipeline.
     */
    ~Shader();
    /** ------------------------------------------------------------------------------------------- Operator()
     * @brief Dispatches one workgroup row per Slice in a list, as the list is; row Y resolves its own Slice.
     * @param slices The Slice holding the Slices, alive until the dispatch retires.
     * @param count Number of Slices in the list.
     * @param done Completion callback invoked once the work is finished.
     * @param context Caller context kept alive through callback return.
     * @return A ShaderResult representing the asynchronous operation.
     */
    ShaderResult operator()(
        const Slice& slices,
        size_t count,
        void (*done)(std::shared_ptr<void>) = nullptr,
        std::shared_ptr<void> context = nullptr
    ) const;
    /** ------------------------------------------------------------------------------------------- Operator()
     * @brief Dispatches one workgroup row whose Slice is the one given.
     * @param slice The Slice, alive until the dispatch retires.
     * @param done Completion callback invoked once the work is finished.
     * @param context Caller context kept alive through callback return.
     * @return A ShaderResult representing the asynchronous operation.
     */
    ShaderResult operator()(
        const Slice& slice,
        void (*done)(std::shared_ptr<void>) = nullptr,
        std::shared_ptr<void> context = nullptr
    ) const;
};
/** --------------------------------------------------------------------------------------------------------- GPU
 * @struct GPU
 * @brief Singleton, static methods for the application's global GPU context. Aggregates backends into a
 * single point of access and control.
 */
struct GPU {
    /** ------------------------------------------------------------------------------------------- Select
     * @brief Chooses the backend the process freezes on; call it before the first Slice or GPU
     * operation, since a different choice after the freeze throws and a set ALLIGATOR_GPU_BACKEND
     * other than auto overrides it.
     * @param backend The backend to freeze on.
     */
    static void select(GPUBackend backend);
    /** ------------------------------------------------------------------------------------------- Backend
     * @brief Returns the backend the process froze on, freezing it now when nothing has yet.
     * @return The active backend.
     */
    static GPUBackend backend();
    /** ------------------------------------------------------------------------------------------- Device Handle
     * @brief Returns a handle to the underlying GPU device.
     * @return A pointer representing the device handle.
     */
    static void* device_handle();
    /** ------------------------------------------------------------------------------------------- Exists
     * @brief Checks if a GPU device exists and is accessible.
     * @return True if a GPU device exists, false otherwise.
     */
    static bool exists();
    /** ------------------------------------------------------------------------------------------- Unified Memory
     * @brief Checks if the GPU supports unified memory.
     * @return True if unified memory is supported, false otherwise.
     */
    static bool unified_memory();
    /** ------------------------------------------------------------------------------------------- Device Name
     * @brief Returns the name of the GPU device.
     * @return A string representing the device name.
     */
    static std::string device_name();
    /** ------------------------------------------------------------------------------------------- Memory Usage
     * @brief Returns the current memory usage of the GPU device.
     * @return A structure representing the device's memory usage.
     */
    static DeviceMemoryUsage memory_usage();
    /** ------------------------------------------------------------------------------------------- Capabilities
     * @brief Returns the frozen device's probed capabilities, every field defaulted without a
     * device.
     * @return The capabilities.
     */
    static GPUCapabilities capabilities();
    /** ------------------------------------------------------------------------------------------- Thread Limits
     * @brief Returns the maximum number of threads per workgroup in each dimension.
     * @return The maximum number of threads in the X dimension.
     */
    static uint32_t thread_limit_x();
    /** ------------------------------------------------------------------------------------------- Thread Limit Y
     * @brief Returns the maximum number of threads per workgroup in the Y dimension.
     * @return The maximum number of threads in the Y dimension.
     */
    static uint32_t thread_limit_y();
    /** ------------------------------------------------------------------------------------------- Thread Limit Z
     * @brief Returns the maximum number of threads per workgroup in the Z dimension.
     * @return The maximum number of threads in the Z dimension.
     */
    static uint32_t thread_limit_z();
    /** ------------------------------------------------------------------------------------------- Workgroup Limit X
     * @brief Returns the maximum number of workgroups in the X dimension.
     * @return The maximum number of workgroups in the X dimension.
     */
    static uint32_t workgroup_limit_x();
    /** ------------------------------------------------------------------------------------------- Workgroup Limit Y
     * @brief Returns the maximum number of workgroups in the Y dimension.
     * @return The maximum number of workgroups in the Y dimension.
     */
    static uint32_t workgroup_limit_y();
    /** ------------------------------------------------------------------------------------------- Workgroup Limit Z
     * @brief Returns the maximum number of workgroups in the Z dimension.
     * @return The maximum number of workgroups in the Z dimension.
     */
    static uint32_t workgroup_limit_z();
};
} // namespace buffetalligator
