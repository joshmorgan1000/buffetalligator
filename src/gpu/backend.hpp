#pragma once
/** --------------------------------------------------------------------------------------------------------- Shader Backend
 * @file backend.hpp
 * @brief Private prepared backend resources shared by the completion and admission engine.
 */
#include <alligator/easygpu.hpp>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Shader Format
 * @brief Identifies the resident parameter layout consumed by a prepared program.
 */
enum class ShaderFormat { Slices, Jobs, References };
/** --------------------------------------------------------------------------------------------------------- GPU Stage
 * @brief Describes one dispatch in a private prepared reference sequence.
 */
struct KernelGpuStage {
    std::string_view glsl;
    uint32_t workgroups_x = 1;
    uint32_t workgroups_y = 1;
    bool per_request = true;
};
/** --------------------------------------------------------------------------------------------------------- Shader Preparation Info
 * @brief Supplies borrowed preparation inputs before a backend creates its immutable program.
 */
struct ShaderPrepareInfo {
    ShaderSource source;
    std::string_view name;
    ShaderFormat format = ShaderFormat::Slices;
    size_t capacity = 1024;
    const Slice* references = nullptr;
    std::span<const KernelGpuStage> stages{};
    uint32_t resources = 0;
    std::span<const uint32_t> words{};
};
/** --------------------------------------------------------------------------------------------------------- Shader Slot
 * @brief Keeps mutable parameters and a borrowed native slot stable through retirement.
 */
struct ShaderSlot {
    Slice parameters;
    uint8_t* mapped = nullptr;
    void* native = nullptr;
    bool busy = false;
};
struct ShaderProgram;
/** --------------------------------------------------------------------------------------------------------- Shader Backend Operations
 * @brief Binds concrete backend functions once during preparation.
 */
struct ShaderBackendOps {
    void (*destroy)(ShaderProgram&) noexcept;
    void (*submit)(ShaderProgram&, ShaderSlot&, uint32_t, uint32_t, uint32_t);
    void (*wait)(ShaderProgram&, ShaderSlot&);
};
/** --------------------------------------------------------------------------------------------------------- Shader Program
 * @brief Owns native resources until every admitted slot has retired.
 */
struct ShaderProgram {
    const ShaderBackendOps* ops = nullptr;
    void* native = nullptr;
    size_t capacity = 0;
    uint32_t max_workgroups_x = 0;
    std::vector<std::unique_ptr<ShaderSlot>> slots;
    std::vector<uint32_t> spirv;
    ~ShaderProgram();
};
std::unique_ptr<ShaderProgram> vulkan_prepare(const ShaderPrepareInfo& info);
std::unique_ptr<ShaderProgram> metal_prepare(const ShaderPrepareInfo& info);
std::string shader_glsl_source(const ShaderPrepareInfo& info);
std::vector<uint32_t> shader_compile_glsl(std::string_view source, bool float16,
    std::string_view name);
void shader_runtime_initialize();
} // namespace buffetalligator
