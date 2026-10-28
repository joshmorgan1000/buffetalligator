/** --------------------------------------------------------------------------------------------------------- Shader Source
 * @file shader_source.cpp
 * @brief Assembles the public Slice shader ABI around a body and compiles GLSL to SPIR-V.
 */
#include <alligator.hpp>
#include <gpu/backend.hpp>
#include <gpu/easyvulkan.hpp>
#include <shaderc/shaderc.hpp>
#include <string>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Public GLSL Tail
 * @brief The pushed address is a list of Slice ids, one per workgroup row; an id resolves through the table.
 */
inline constexpr std::string_view PUBLIC_GLSL_TAIL = R"glsl(
uint64_t slice_address(uint id) { U64Array table = U64Array(uint64_t(EZ_TABLE)); return table.v[id]; }
uint vulkan_slice(uint index) { U32Array list = U32Array(push.address); return list.v[index]; }
uint vulkan_index() { return gl_WorkGroupID.y; }
uint vulkan_count() { return gl_NumWorkGroups.y; }
)glsl";
/** --------------------------------------------------------------------------------------------------------- Public Shader Source
 * @brief The prelude, the table's address baked in, the id helpers, the body and the entry point.
 * @param body The body of the shader defining `void alligator_main(uint slice)` and its workgroup shape.
 * @return The complete GLSL source.
 */
std::string public_shader_source(std::string_view body) {
    static_cast<void>(IBuffet::current());
    constexpr std::string_view entry = "\nvoid main() { alligator_main(vulkan_slice(vulkan_index())); }\n";
    std::string source;
    source.reserve(64 + EZ_GLSL_PRELUDE.size() + PUBLIC_GLSL_TAIL.size() + body.size() + entry.size());
    source.append(EZ_GLSL_PRELUDE);
    source.append("#define EZ_TABLE ").append(std::to_string(IBuffet::device_table())).append("ul\n");
    source.append(PUBLIC_GLSL_TAIL);
    source.append(body);
    source.append(entry);
    return source;
}
/** --------------------------------------------------------------------------------------------------------- Compile GLSL
 * @brief Compiles GLSL source code into Vulkan SPIR-V.
 * @param source GLSL source code.
 * @param float16 Whether to enable 16-bit floating point support.
 * @param name Diagnostic name for error reporting.
 * @return The SPIR-V words.
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
