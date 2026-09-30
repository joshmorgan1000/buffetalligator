#pragma once
/** --------------------------------------------------------------------------------------------------------- EasyGPU
 * @file easygpu.hpp
 * @brief Portable prepared shaders with owned completion results and coroutine suspension.
 */
#include <logging.hpp>
#include <alligator.hpp>
#include <cstddef>
#include <cstdint>
#include <coroutine>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- GPU Exception
 * @brief Reports shader preparation, admission, execution, or completion failures.
 */
class GPUException : public threadsafe_logger::Exception {
public:
    using threadsafe_logger::Exception::Exception;
    using threadsafe_logger::Exception::what;
};
#define ALLIGATOR_GPU_THROW(msg) throw GPUException(msg)
/** --------------------------------------------------------------------------------------------------------- Device Memory Usage
 * @brief Reports native device measurements with unavailable quantities left empty.
 */
struct DeviceMemoryUsage {
    std::optional<uint64_t> capacity_bytes;
    std::optional<uint64_t> available_bytes;
    std::optional<uint64_t> process_bytes;
    std::optional<uint64_t> budget_bytes;
};
/** --------------------------------------------------------------------------------------------------------- Shader Source
 * @brief Supplies owned-at-preparation native bodies and the portable GLSL body.
 */
struct ShaderSource {
    std::string_view glsl;
    std::string_view metal;
    std::string_view cuda;
};
class ShaderState;
struct ShaderOperation;
class ShaderAwaiter;
/** --------------------------------------------------------------------------------------------------------- Shader Result
 * @brief Owns completion state independently of the Shader, its callback, and suspended frame.
 */
class ShaderResult {
private:
    std::shared_ptr<ShaderOperation> operation_;
    friend class Shader;
    friend class ShaderAwaiter;
public:
    ShaderResult();
    ShaderResult(const ShaderResult&);
    ShaderResult& operator=(const ShaderResult&);
    ShaderResult(ShaderResult&&) noexcept;
    ShaderResult& operator=(ShaderResult&&) noexcept;
    ~ShaderResult();
    /** ------------------------------------------------------------------------------------------- Ready
     * @brief Reports retirement after device writes become host-visible.
     */
    bool ready() const;
    /** ------------------------------------------------------------------------------------------- Rethrow
     * @brief Rethrows a completed operation's failure and rejects an unready result.
     */
    void rethrow() const;
    /** ------------------------------------------------------------------------------------------- Cancel
     * @brief Detaches suspension and waits for any executing continuation before frame destruction.
     */
    void cancel() const;
};
/** --------------------------------------------------------------------------------------------------------- Shader Awaiter
 * @brief Suspends on owned work whose external frame owner must cancel before concurrent destruction.
 */
class ShaderAwaiter {
private:
    ShaderResult result_;
    explicit ShaderAwaiter(ShaderResult result);
    friend class Shader;
public:
    ShaderAwaiter(const ShaderAwaiter&) = delete;
    ShaderAwaiter& operator=(const ShaderAwaiter&) = delete;
    ShaderAwaiter(ShaderAwaiter&&) noexcept;
    ShaderAwaiter& operator=(ShaderAwaiter&&) noexcept;
    ~ShaderAwaiter();
    bool await_ready() const;
    bool await_suspend(std::coroutine_handle<> continuation);
    void await_resume();
    /** ------------------------------------------------------------------------------------------- Result
     * @brief Returns the cancellation handle the frame owner retains before starting suspension.
     */
    ShaderResult result() const;
};
/** --------------------------------------------------------------------------------------------------------- Shader
 * @brief Prepares one backend program and admits asynchronous dispatches with retained Slice identities.
 */
class Shader {
private:
    std::unique_ptr<ShaderState> state_;
    explicit Shader(std::unique_ptr<ShaderState> state);
    friend struct GPU;
public:
    explicit Shader(const ShaderSource& source, std::string_view name = "alligator_shader");
    explicit Shader(std::string_view glsl, std::string_view name = "alligator_shader");
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&&) noexcept;
    Shader& operator=(Shader&&) noexcept;
    ~Shader();
    /** ------------------------------------------------------------------------------------------- Dispatch
     * @brief Accepts zero-copy work and invokes done once on Kitchen after every round retires.
     * @param slices Bound Slice identities retained before admission returns.
     * @param count Number of bound slices.
     * @param result Owned terminal state published before done executes.
     * @param done Completion callback whose exceptions are retained in result.
     * @param context Caller context kept alive through callback return.
     * @param workgroups Number of workgroups along X per Slice.
     * @param dependencies Additional Slice identities embedded in bound payloads.
     */
    void operator()(const Slice* slices, size_t count, ShaderResult& result,
        void (*done)(void*) = nullptr, void* context = nullptr, uint32_t workgroups = 1,
        std::span<const Slice> dependencies = {}) const;
    void operator()(const Slice& slice, ShaderResult& result,
        void (*done)(void*) = nullptr, void* context = nullptr, uint32_t workgroups = 1,
        std::span<const Slice> dependencies = {}) const;
    /** ------------------------------------------------------------------------------------------- Awaitable Dispatch
     * @brief Admits retained work whose continuation resumes on a Kitchen worker.
     */
    ShaderAwaiter dispatch(const Slice* slices, size_t count, uint32_t workgroups = 1,
        std::span<const Slice> dependencies = {}) const;
};
static_assert(sizeof(Shader) == sizeof(void*), "Shader must remain one opaque pointer.");
/** --------------------------------------------------------------------------------------------------------- GPU
 * @brief Prepares portable programs and reports the active compute device.
 */
struct GPU {
    static bool exists();
    static bool unified_memory();
    static std::string device_name();
    static DeviceMemoryUsage memory_usage();
    /** ------------------------------------------------------------------------------------------- Encode
     * @brief Serializes versioned little-endian BAGP source fields with eight-byte payload alignment.
     */
    static Slice encode(const Shader& program);
    /** ------------------------------------------------------------------------------------------- Decode
     * @brief Validates a portable source record and prepares it for the active device.
     */
    static Shader decode(const Slice& program);
    static void run(const Shader& program, const Slice* streams, size_t count, ShaderResult& result,
        void (*done)(void*) = nullptr, void* context = nullptr,
        std::span<const Slice> dependencies = {});
    static void run(const Slice& program, const Slice* streams, size_t count, ShaderResult& result,
        void (*done)(void*) = nullptr, void* context = nullptr,
        std::span<const Slice> dependencies = {});
    static ShaderAwaiter dispatch(const Shader& program, const Slice* streams, size_t count,
        std::span<const Slice> dependencies = {});
    static ShaderAwaiter dispatch(const Slice& program, const Slice* streams, size_t count,
        std::span<const Slice> dependencies = {});
    /** ------------------------------------------------------------------------------------------- Compile GLSL
     * @brief Prepares a job-table main body and returns its portable encoded source record.
     */
    static Slice compile_glsl(std::string_view body);
};
} // namespace buffetalligator
