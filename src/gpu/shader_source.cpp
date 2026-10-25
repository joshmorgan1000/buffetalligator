/** --------------------------------------------------------------------------------------------------------- Shader Source
 * @file shader_source.cpp
 * @brief Assembles the public Slice shader ABI and compiles GLSL to SPIR-V.
 */
#include <gpu/backend.hpp>
#include <shaderc/shaderc.hpp>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Public GLSL Tail
 * @brief The push block carries one Slice; it is the row's Slice, or a list whose element Y is the row's
 * Slice.
 */
inline constexpr std::string_view PUBLIC_GLSL_TAIL = R"glsl(
Slice vulkan_slice(uint index) {
    if (vulkan_push.vulkan_slice_list == 0u) return vulkan_push.vulkan_slice;
    return slice_read(slice_address(vulkan_push.vulkan_slice) + uint64_t(index) * 64ul);
}
uint vulkan_index() { return gl_WorkGroupID.y; }
uint vulkan_count() { return gl_NumWorkGroups.y; }
layout(local_size_x = 16, local_size_y = 4, local_size_z = 1) in;
)glsl";
/** --------------------------------------------------------------------------------------------------------- Public Shader Source
 * @brief Assembles the injected Slice ABI, fixed workgroup shape, user function, and entry point.
 * @param body The body of the shader defining `void alligator_main(Slice)`.
 * @return The complete GLSL source code as a string.
 */
std::string public_shader_source(std::string_view body) {
    constexpr std::string_view entry = "\nvoid main() { alligator_main(vulkan_slice(vulkan_index())); }\n";
    std::string source;
    source.reserve(13 + VULKAN_GLSL_KERNEL_CORE.size() + PUBLIC_GLSL_TAIL.size() + body.size() + entry.size());
    source.append("#version 450\n");
    source.append(VULKAN_GLSL_KERNEL_CORE);
    source.append(PUBLIC_GLSL_TAIL);
    source.append(body);
    source.append(entry);
    return source;
}
/** --------------------------------------------------------------------------------------------------------- Compile GLSL
 * @brief Compiles GLSL source code into Vulkan SPIR-V binary format.
 * @param source GLSL source code as a string view.
 * @param float16 Whether to enable 16-bit floating point support.
 * @param name Diagnostic name for error reporting.
 * @return A vector of 32-bit words representing the compiled SPIR-V binary.
 */
std::vector<uint32_t> shader_compile_glsl(std::string_view source, bool float16, std::string_view name) {
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    if (float16) options.AddMacroDefinition("VULKAN_FLOAT16", "1");
    const std::string diagnostic_name(name);
    const shaderc::SpvCompilationResult result = compiler.CompileGlslToSpv(
        source.data(), source.size(), shaderc_compute_shader, diagnostic_name.c_str(), options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        ALLIGATOR_GPU_THROW("GLSL shader compile failed [" + diagnostic_name + "]: " + result.GetErrorMessage());
    }
    return std::vector<uint32_t>(result.cbegin(), result.cend());
}
} // namespace buffetalligator
