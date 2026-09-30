#pragma once
/** --------------------------------------------------------------------------------------------------------- Shader State
 * @file shader_state.hpp
 * @brief Private prepared programs and retained asynchronous dispatch state.
 */
#include <alligator/easygpu.hpp>
#include <gpu/backend.hpp>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- ShaderState
 * @class ShaderState
 * @brief Shares immutable pipelines across independently admitted submission slots.
 */
class ShaderState {
    struct Impl;
    std::shared_ptr<Impl> impl_;
    friend struct ShaderOperation;
    friend struct GPU;
    friend class Shader;
public:
    ShaderState(const ShaderSource& source, std::string_view name, bool jobs = false);
    static void drain();
    std::shared_ptr<ShaderOperation> start(const Slice* streams, size_t count,
        void (*done)(void*), void* context, uint32_t workgroups,
        std::span<const Slice> dependencies) const;
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
    void dispatch(const Slice* streams, size_t count, uint32_t workgroups) const;
    /** ------------------------------------------------------------------------------------------- Dispatch References
     * @brief Dispatches the reference ring over a range.
     * @param first The first reference index.
     * @param count The reference count.
     */
    void dispatch_references(uint32_t first, uint32_t count);
};
} // namespace buffetalligator
