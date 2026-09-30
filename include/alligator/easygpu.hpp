#pragma once
/** --------------------------------------------------------------------------------------------------------- EasyGPU
 * @file include/alligator/easygpu.hpp
 * @brief EasyGPU utilities for the Alligator library.
 */
#include <logging.hpp>
#include <alligator.hpp>
#include <alligator/easyvulkan.hpp>
#include <semaphore>
#include <memory>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- GPUException
 * @class GPUException
 * @brief A GPUException is thrown when a GPU operation fails.
 */
class GPUException : public threadsafe_logger::Exception {
public:
    using threadsafe_logger::Exception::Exception;
    using threadsafe_logger::Exception::what;
};
#define ALLIGATOR_GPU_THROW(msg) throw GPUException(msg)
/** --------------------------------------------------------------------------------------------------------- Device Memory Usage
 * @struct DeviceMemoryUsage
 * @brief Reports native device measurements with unavailable quantities left empty.
 */
struct DeviceMemoryUsage {
    std::optional<uint64_t> capacity_bytes;
    std::optional<uint64_t> available_bytes;
    std::optional<uint64_t> process_bytes;
    std::optional<uint64_t> budget_bytes;
};
/// @brief Forward declaration of the internal shader state used by the Shader class.
class ShaderState;
/** --------------------------------------------------------------------------------------------------------- Shader
 * @class Shader
 * @brief Prepared `alligator_main(Slice)` GLSL: a call binds a list of slices and each workgroup
 * column processes its own slice in place.
 */
class Shader {
private:
    /// @brief Internal state of the shader, managed by the Shader class.
    std::unique_ptr<ShaderState> state_;
public:
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Compiles GLSL and prepares every Vulkan resource used by subsequent calls.
     * @param source GLSL defining `void alligator_main(Slice slice)`, the slice this workgroup owns.
     * @param name Diagnostic name reported by shader compilation errors.
     */
    explicit Shader(
        std::string_view source,
        std::string_view name = "alligator_shader"
    );
    /** ------------------------------------------------------------------------------------------- Move-only ownership */
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&& other) noexcept;
    Shader& operator=(Shader&& other) noexcept;
    /** ------------------------------------------------------------------------------------------- Destructor */
    ~Shader();
    /** ------------------------------------------------------------------------------------------- Shader Functor invocation
     * @brief Runs the shader over a list of slices: Y is shaped to the list's length and
     * workgroup column `i` receives `slices[i]`, writing its results into that slice.
     * @param slices Device-visible slices, one per workgroup column.
     * @param count How many slices are bound.
     * @param callback Optional function invoked with each slice once the shader has completed.
     * @param workgroups Number of 64-invocation workgroups along X per slice.
     * @return A shared pointer to a LightweightSemaphore that can optionally be waited on until
     * the shader has completed execution.
     */
    std::binary_semaphore operator()(
        const Slice* slices,
        size_t count,
        void (*callback)(Slice slice) = nullptr,
        uint32_t workgroups = 1
    ) const;
    /** ------------------------------------------------------------------------------------------- Shader Functor invocation (one)
     * @brief Runs the shader over a single slice.
     * @param slice The device-visible slice, processed in place.
     * @param callback Optional function invoked with the slice once the shader has completed.
     * @param workgroups Number of 64-invocation workgroups along X.
     * @return A binary semaphore that can optionally be waited on until the shader has completed
     * execution.
     */
    std::binary_semaphore operator()(
        const Slice& slice,
        void (*callback)(Slice slice) = nullptr,
        uint32_t workgroups = 1
    ) const;
};
static_assert(sizeof(Shader) == sizeof(void*), "Shader's public ABI must remain one opaque pointer.");
/** --------------------------------------------------------------------------------------------------------- GPU struct
 * @struct GPU
 * @brief Encapsulates static methods used for GPU compute operations.
 */
struct GPU {
    /** ------------------------------------------------------------------------------------------- available
     * @brief True when a Vulkan compute device is present.
     * @return True when the GPU can execute programs.
     */
    static bool exists();
    /** ------------------------------------------------------------------------------------------- unified_memory
     * @brief True when the device shares memory with the CPU, so stream Slices bind with no
     * copies.
     * @return True under unified memory.
     */
    static bool unified_memory();
    /** ------------------------------------------------------------------------------------------- device_name
     * @brief The compute device's name, empty when no device is present.
     * @return The device name.
     */
    static std::string device_name();
    /** ------------------------------------------------------------------------------------------- encode
     * @brief Flattens a recorded program into one Slice: header, register seeds, constants, then
     * the instruction stream as a contiguous run of 8-byte instructions.
     * @param program The recorded program.
     * @return The encoded program; place it, move it, or hand it to run() below like any Slice.
     */
    static Slice encode(const Shader& program);
    /** ------------------------------------------------------------------------------------------- decode
     * @brief Rebuilds a recorded program from its encoded Slice.
     * @param program An encoded program.
     * @return The program.
     * @throw ShaderException when the Slice is not an encoded program of this version.
     */
    static Shader decode(const Slice& program);
    /** ------------------------------------------------------------------------------------------- run
     * @brief Executes a recorded program on the GPU over the bound streams.
     * @param program The recorded program.
     * @param streams One Slice per workgroup column; column i processes streams[i] in place.
     * @param stream_count How many Slices are bound; becomes the dispatch's Y extent.
     */
    static void run(const Shader& program, Slice* streams, size_t stream_count);
    /** ------------------------------------------------------------------------------------------- run
     * @brief Decodes an encoded program and executes it on the GPU.
     * @param program A compile_glsl program (job-table engine, one job per Slice).
     * @param streams One Slice per job.
     * @param stream_count How many jobs this round binds.
     */
    static void run(const Slice& program, Slice* streams, size_t stream_count);
    /** ------------------------------------------------------------------------------------------- compile_glsl
     * @brief Compiles a kernel body under the alligator prelude into SPIR-V words held in a Slice.
     * The body defines main() against the prelude's Slice-addressing helpers; the workgroup shape
     * (16x4) and the 8-byte job-table push block are injected.
     * @param body GLSL defining main(); the shape is injected, not authored.
     * @return The SPIR-V words, one per four bytes.
     * @throw GPUException when compilation fails or no device is present.
     */
    static Slice compile_glsl(std::string_view body);
};
} // namespace alligator