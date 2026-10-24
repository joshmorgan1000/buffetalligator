/** --------------------------------------------------------------------------------------------------------- Shader Source
 * @file shader_source.cpp
 * @brief Assembles the public Chomp shader ABI and compiles GLSL to SPIR-V.
 */
#include <gpu/backend.hpp>
#include <shaderc/shaderc.hpp>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Public GLSL Tail
 * @brief The push block carries one Chomp; it is the row's Chomp, or a list whose element Y is the row's
 * Chomp, resolved through the directory like every other id.
 */
inline constexpr std::string_view PUBLIC_GLSL_TAIL = R"glsl(
Chomp vulkan_chomp(uint index) {
    Chomp pushed = Chomp(vulkan_push.vulkan_chomp_id);
    if (vulkan_push.vulkan_chomp_list == 0u) return pushed;
    return chomp_read(chomp_address(pushed) + uint64_t(index) * 4ul);
}
uint vulkan_index() { return gl_WorkGroupID.y; }
uint vulkan_count() { return gl_NumWorkGroups.y; }
layout(local_size_x = 16, local_size_y = 4, local_size_z = 1) in;
)glsl";
/** --------------------------------------------------------------------------------------------------------- Public Shader Source
 * @brief Assembles the injected Chomp ABI, fixed workgroup shape, user function, and entry point.
 * @param body The body of the shader defining `void alligator_main(Chomp)`.
 * @return The complete GLSL source code as a string.
 */
std::string public_shader_source(std::string_view body) {
    constexpr std::string_view entry = "\nvoid main() { alligator_main(vulkan_chomp(vulkan_index())); }\n";
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
