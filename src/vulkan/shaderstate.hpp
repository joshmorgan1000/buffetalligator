#pragma once
/** --------------------------------------------------------------------------------------------------------- ShaderState
 * @file shaderstate.hpp
 * @brief Shared prepared dispatch state for Shader and Kernel reference rings.
 */
#include <alligator.hpp>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace buffetalligator {
class GPUSlice;
/** --------------------------------------------------------------------------------------------------------- GPU Stage
 * @struct KernelGpuStage
 * @brief One dispatch in a Kernel's prepared GPU command sequence.
 */
struct KernelGpuStage {
    std::string_view glsl;      ///< The stage's GLSL source.
    uint32_t workgroups_x = 1;  ///< Workgroups along X.
    uint32_t workgroups_y = 1;  ///< Workgroups along Y.
    bool per_request = true;    ///< Whether the stage dispatches once per request.
};
/** --------------------------------------------------------------------------------------------------------- ShaderState
 * @class ShaderState
 * @brief Shared prepared resources for Shader, legacy jobs, and Kernel reference rings.
 * One controller owns an instance; dispatches on the same instance must not overlap.
 */
class ShaderState {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    /** ------------------------------------------------------------------------------------------- Constructor - Source
     * @brief Compiles GLSL and prepares every Vulkan resource used by subsequent dispatches.
     * @param source GLSL defining `void alligator_main(Slice slice)`.
     * @param name Diagnostic name reported by shader compilation errors.
     * @param references Optional reference list the shader indexes.
     * @param workgroups_x Workgroups along X for reference dispatches.
     */
    ShaderState(std::string_view source, std::string_view name,
        const Slice* references = nullptr, uint32_t workgroups_x = 1);
    /** ------------------------------------------------------------------------------------------- Constructor - Words
     * @brief Prepares dispatch state from already-compiled SPIR-V words.
     * @param words The SPIR-V words.
     * @param count The word count.
     * @param name Diagnostic name.
     * @param max_jobs The job-table capacity.
     */
    ShaderState(const uint32_t* words, size_t count, std::string_view name, size_t max_jobs);
    /** ------------------------------------------------------------------------------------------- Constructor - Stages
     * @brief Compiles and chains one dispatch per stage for Kernel command sequences.
     * @param stages The stage descriptors.
     * @param name Diagnostic name.
     * @param references The reference list the stages index.
     * @param resources The resource count published to the stages.
     */
    ShaderState(std::span<const KernelGpuStage> stages, std::string_view name,
        const Slice& references, uint32_t resources);
    ~ShaderState();
    /** ------------------------------------------------------------------------------------------- SPIR-V
     * @brief The compiled SPIR-V words.
     * @return The words.
     */
    const std::vector<uint32_t>& spirv() const;
    /** ------------------------------------------------------------------------------------------- Name
     * @brief The diagnostic name.
     * @return The name.
     */
    const std::string& name() const;
    /** ------------------------------------------------------------------------------------------- Capacity
     * @brief The job-table capacity.
     * @return The capacity.
     */
    size_t capacity() const;
    /** ------------------------------------------------------------------------------------------- Dispatch
     * @brief Binds one slice per workgroup column and dispatches.
     * @param streams The slices to bind.
     * @param count The stream count.
     * @param workgroups The workgroup count.
     */
    void dispatch(const GPUSlice* streams, size_t count, uint32_t workgroups) const;
    /** ------------------------------------------------------------------------------------------- Write Job
     * @brief Publishes one job-table slot.
     * @param slot The slot index.
     * @param input The job's input slice.
     * @param host_handle The host handle mirrored to the device.
     */
    void write_job(size_t slot, const Slice& input, const void* host_handle);
    /** ------------------------------------------------------------------------------------------- Dispatch Jobs
     * @brief Dispatches the megakernel over the job table.
     * @param jobs The number of published jobs.
     */
    void dispatch(size_t jobs);
    /** ------------------------------------------------------------------------------------------- Dispatch References
     * @brief Dispatches the reference ring over a range.
     * @param first The first reference index.
     * @param count The reference count.
     */
    void dispatch_references(uint32_t first, uint32_t count);
};
} // namespace buffetalligator
