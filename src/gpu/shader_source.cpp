/** --------------------------------------------------------------------------------------------------------- Shader Source
 * @file shader_source.cpp
 * @brief Assembles the portable shader ABI and compiles shared SPIR-V preparation artifacts.
 */
#include <gpu/backend.hpp>
#include <alligator/easyvulkan.hpp>
#include <shaderc/shaderc.hpp>

namespace buffetalligator {
namespace {
/** --------------------------------------------------------------------------------------------------------- Public GLSL Tail
 * @brief Resolves the parameter list's pool ID and hands workgroup Y its own slice.
 */
inline constexpr std::string_view PUBLIC_GLSL_TAIL = R"glsl(
Slice vulkan_list() { return slice_read(vulkan_push.vulkan_table_address); }
uint vulkan_count() { return gl_NumWorkGroups.y; }
Slice vulkan_slice(uint index) { return slice_read(slice_address(vulkan_list()) + uint64_t(index) * 4ul); }
uint vulkan_index() { return gl_WorkGroupID.y; }
layout(local_size_x = 16, local_size_y = 4, local_size_z = 1) in;
)glsl";
/** --------------------------------------------------------------------------------------------------------- Public Shader Source
 * @brief Assembles the injected Slice ABI, fixed workgroup shape, user function, and entry point.
 * @param body The body of the shader function to be injected into the final source.
 * @return The complete GLSL source code as a string.
 */
std::string public_shader_source(std::string_view body) {
    constexpr std::string_view entry =
        "\nvoid main() { alligator_main(vulkan_slice(vulkan_index())); }\n";
    std::string source;
    source.reserve(13 + VULKAN_GLSL_KERNEL_CORE.size() + PUBLIC_GLSL_TAIL.size()
        + body.size() + entry.size());
    source.append("#version 450\n");
    source.append(VULKAN_GLSL_KERNEL_CORE);
    source.append(PUBLIC_GLSL_TAIL);
    source.append(body);
    source.append(entry);
    return source;
}
/** --------------------------------------------------------------------------------------------------------- reference_shader_source
 * @brief Generates the reference shader source code.
 * @param body The body of the shader.
 * @return The complete GLSL source code for the reference shader.
 */
std::string reference_shader_source(std::string_view body) {
    std::string source("#version 450\n");
    source.append(VULKAN_GLSL_KERNEL_CORE);
    source.append(R"glsl(
layout(local_size_x = 16, local_size_y = 4, local_size_z = 1) in;
uint vulkan_index() { return gl_WorkGroupID.z; }
Slice vulkan_resources() { return gpu_slice(U32Array(vulkan_push.vulkan_table_address).v[7]); }
uint vulkan_request() {
    uint64_t table = vulkan_push.vulkan_table_address;
    uint mask = U32Array(table).v[2];
    uint first = U32Array(table).v[3];
    return U32Array(U64Array(table).v[0]).v[(first + vulkan_index()) & mask];
}
)glsl");
    source.append(body);
    source.append(R"glsl(
void main() {
    Slice invocation = gpu_slice(vulkan_request());
    alligator_main(gpu_slice(slice_load_u32(invocation, 0u)), gpu_slice(slice_load_u32(invocation, 1u)));
}
)glsl");
    return source;
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Compile Vulkan GLSL
 * @brief Compiles GLSL source code into Vulkan SPIR-V binary format.
 * @param source GLSL source code as a string view.
 * @param float16 Whether to enable 16-bit floating point support.
 * @param name Diagnostic name for error reporting.
 * @return A vector of 32-bit words representing the compiled SPIR-V binary.
 */
std::vector<uint32_t> shader_compile_glsl(
    std::string_view source,
    bool float16,
    std::string_view name
) {
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    if (float16) options.AddMacroDefinition("VULKAN_FLOAT16", "1");
    const std::string diagnostic_name(name);
    const shaderc::SpvCompilationResult result = compiler.CompileGlslToSpv(
        source.data(), source.size(), shaderc_compute_shader, diagnostic_name.c_str(), options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        ALLIGATOR_GPU_THROW("GLSL shader compile failed [" + diagnostic_name + "]: "
            + result.GetErrorMessage());
    }
    return std::vector<uint32_t>(result.cbegin(), result.cend());
}
/** --------------------------------------------------------------------------------------------------------- Shader GLSL Source
 * @brief Selects the resident parameter ABI before compiling a portable source body.
 */
std::string shader_glsl_source(const ShaderPrepareInfo& info) {
    if (info.format == ShaderFormat::Jobs) return kernel_shader_source(info.source.glsl);
    if (info.format == ShaderFormat::References) return reference_shader_source(info.source.glsl);
    return public_shader_source(info.source.glsl);
}
} // namespace buffetalligator
