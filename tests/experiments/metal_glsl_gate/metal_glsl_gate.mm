/** --------------------------------------------------------------------------------------------------------- Metal GLSL Gate
 * @file metal_glsl_gate.mm
 * @brief Executes the production Slice GLSL preludes through pinned SPIRV-Cross and native Metal.
 */
#include <alligator.hpp>
#include <alligator/easyvulkan.hpp>
#include <shaderc/shaderc.hpp>
#include <spirv_msl.hpp>
#include <Foundation/Foundation.h>
#include <Metal/Metal.h>
#include <simd/simd.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

using namespace buffetalligator;
namespace {
/** --------------------------------------------------------------------------------------------------------- Require
 * @brief Reports a failed experimental gate without compiling checks out of optimized builds.
 */
void require(bool passed, const std::string& message) {
    if (!passed) throw std::runtime_error(message);
}
/** --------------------------------------------------------------------------------------------------------- Save Text
 * @brief Preserves complete generated source and compiler diagnostics for inspection.
 */
void save_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path);
    output << text;
    require(bool(output), "Could not write experiment artifact " + path.string());
}
/** --------------------------------------------------------------------------------------------------------- Public Source
 * @brief Reads the current production public-list tail without duplicating its Slice lookup logic.
 */
std::string public_source(const std::string& body) {
    std::ifstream input(std::string(ALLIGATOR_GATE_SOURCE_ROOT) + "/src/gpu/shader_source.cpp");
    const std::string implementation{std::istreambuf_iterator<char>(input), {}};
    const size_t declaration = implementation.find("std::string_view PUBLIC_GLSL_TAIL");
    require(declaration != std::string::npos, "Production public-list prelude declaration is missing");
    const size_t begin = implementation.find("R\"glsl(", declaration);
    const size_t end = implementation.find(")glsl\"", begin);
    require(begin != std::string::npos && end != std::string::npos,
        "Production public-list prelude literal is incomplete");
    std::string source("#version 450\n");
    source.append(VULKAN_GLSL_KERNEL_CORE);
    source.append(implementation, begin + 7, end - begin - 7);
    source.append(body);
    source.append("\nvoid main() { alligator_main(vulkan_slice(vulkan_index())); }\n");
    return source;
}
/** --------------------------------------------------------------------------------------------------------- Prepare Pipeline
 * @brief Compiles the real GLSL ABI and explicitly maps its 16-byte push block to Metal buffer zero.
 */
id<MTLComputePipelineState> prepare(id<MTLDevice> device, const std::string& source,
    const std::string& name, const std::filesystem::path& artifacts
) {
    LOG_INFO_STREAM << "Compiling " << name << " production GLSL through shaderc and pinned SPIRV-Cross";
    save_text(artifacts / (name + ".glsl"), source);
    shaderc::Compiler frontend;
    shaderc::CompileOptions options;
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    const auto compiled = frontend.CompileGlslToSpv(source, shaderc_compute_shader, name.c_str(), options);
    save_text(artifacts / (name + ".shaderc.txt"), compiled.GetErrorMessage());
    require(compiled.GetCompilationStatus() == shaderc_compilation_status_success,
        "shaderc failed for " + name + ": " + compiled.GetErrorMessage());
    const std::vector<uint32_t> words(compiled.cbegin(), compiled.cend());
    std::ofstream binary(artifacts / (name + ".spv"), std::ios::binary);
    binary.write(reinterpret_cast<const char*>(words.data()), words.size() * sizeof(uint32_t));
    require(bool(binary), "Could not preserve generated SPIR-V");
    spirv_cross::CompilerMSL translator(words);
    auto settings = translator.get_msl_options();
    settings.platform = spirv_cross::CompilerMSL::Options::macOS;
    settings.set_msl_version(3, 0);
    translator.set_msl_options(settings);
    const auto resources = translator.get_shader_resources();
    require(resources.push_constant_buffers.size() == 1, "Push-block count differs from production ABI");
    const auto& push_type = translator.get_type(resources.push_constant_buffers[0].base_type_id);
    require(translator.get_declared_struct_size(push_type) == 16
        && translator.type_struct_member_offset(push_type, 0) == 0
        && translator.type_struct_member_offset(push_type, 1) == 8,
        "Translated push-block size or field offsets changed");
    spirv_cross::MSLResourceBinding push;
    push.stage = spv::ExecutionModelGLCompute;
    push.desc_set = spirv_cross::ResourceBindingPushConstantDescriptorSet;
    push.binding = spirv_cross::ResourceBindingPushConstantBinding;
    push.msl_buffer = 0;
    translator.add_msl_resource_binding(push);
    translator.rename_entry_point("main", "alligator_entry", spv::ExecutionModelGLCompute);
    const std::string msl = translator.compile();
    save_text(artifacts / (name + ".metal"), msl);
    LOG_INFO_STREAM << "Compiling " << name << " generated MSL3.0 and preparing the native pipeline";
    NSError* error = nil;
    MTLCompileOptions* metal_options = [MTLCompileOptions new];
    metal_options.languageVersion = MTLLanguageVersion3_0;
    id<MTLLibrary> library = [device newLibraryWithSource:@(msl.c_str()) options:metal_options error:&error];
    save_text(artifacts / (name + ".metal.txt"), error ? error.description.UTF8String : "");
    require(library != nil, "MSL compilation failed for " + name + ": "
        + (error ? std::string(error.description.UTF8String) : "no diagnostic"));
    if (error) LOG_WARN_STREAM << name << " Metal diagnostics: " << error.description.UTF8String;
    id<MTLFunction> function = [library newFunctionWithName:@"alligator_entry"];
    require(function != nil, "Translated entry point was not found");
    error = nil;
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    save_text(artifacts / (name + ".pipeline.txt"), error ? error.description.UTF8String : "");
    require(pipeline != nil, "Pipeline creation failed for " + name + ": "
        + (error ? std::string(error.description.UTF8String) : "no diagnostic"));
    const MTLSize maximum = device.maxThreadsPerThreadgroup;
    require(maximum.width >= 16 && maximum.height >= 4 && maximum.depth >= 1
        && pipeline.maxTotalThreadsPerThreadgroup >= 64
        && pipeline.staticThreadgroupMemoryLength <= device.maxThreadgroupMemoryLength,
        "Device/pipeline limits cannot execute the production 16x4x1 workgroup");
    LOG_INFO_STREAM << name << " prepared: SPIR-V words=" << words.size()
        << " MSL3.0, push=16 bytes at buffer0, pipeline width=" << pipeline.threadExecutionWidth
        << " max threads=" << pipeline.maxTotalThreadsPerThreadgroup
        << " shared bytes=" << pipeline.staticThreadgroupMemoryLength;
    return pipeline;
}
/** --------------------------------------------------------------------------------------------------------- Slice Identifier
 * @brief Encodes the production slot/region/placement bit layout for Metal-owned test allocations.
 */
constexpr uint32_t identifier(uint32_t slot, uint32_t region) { return (slot << 9) | (region << 3) | 2; }
/** --------------------------------------------------------------------------------------------------------- Push Block
 * @brief Mirrors the production physical-address push block validated against SPIR-V reflection.
 */
struct PushBlock { uint64_t table; uint64_t directory; };
static_assert(sizeof(PushBlock) == 16 && offsetof(PushBlock, directory) == 8);
/** --------------------------------------------------------------------------------------------------------- Run Gate
 * @brief Checks public-list and job-table execution using retained native resources and exact outputs.
 */
void run(const std::filesystem::path& artifacts) {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    require(device != nil, "No native Metal device is available");
    require(device.hasUnifiedMemory && device.argumentBuffersSupport == MTLArgumentBuffersTier2,
        "This gate requires a probed unified-memory Tier2 device for shared physical pointers");
    LOG_INFO_STREAM << "Metal device=" << device.name.UTF8String
        << " maxBufferLength=" << device.maxBufferLength
        << " maxThreadgroupMemory=" << device.maxThreadgroupMemoryLength
        << " argumentBuffersTier=" << device.argumentBuffersSupport
        << " Apple9=" << bool([device supportsFamily:MTLGPUFamilyApple9]);
    LOG_INFO_STREAM << "Host OS=" << NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String;
    LOG_INFO_STREAM << "Pinned SPIRV-Cross=" << ALLIGATOR_GATE_CROSS_REVISION
        << " shaderc target=Vulkan1.2 optimization=performance MSL target=macOS3.0";
    const std::string body = R"glsl(
void alligator_main(Slice parameters) {
    if (gl_LocalInvocationIndex != 0u) return;
    uvec4 fields = slice_load_u32x4(parameters, 0u);
    Slice input_slice = gpu_slice(fields.x);
    Slice output_slice = gpu_slice(fields.y);
    slice_store_f32x4(output_slice, 0u, slice_load_f32x4(input_slice, 0u) * 2.0 + vec4(float(fields.w)));
    slice_store_u32(output_slice, 4u, uint(slice_size(input_slice)));
    slice_store_u32(output_slice, 5u, uint(slice_size(parameters)));
    slice_store_u32(output_slice, 6u, slice_is_null(gpu_slice(0xFFFFFFFFu)) ? 1u : 0u);
    slice_store_u32(output_slice, 7u, gate_count());
    slice_store_u64(output_slice, 4u, slice_address(input_slice));
    slice_store_u32x4(output_slice, 3u, uvec4(fields.z, fields.w, uint(slice_size(output_slice)), gate_count()));
}
)glsl";
    const std::string public_glsl = public_source("uint gate_count() { return vulkan_count(); }\n" + body);
    const std::string jobs_glsl = kernel_shader_source(
        "uint gate_count() { return vulkan_job_count(); }\n" + body
        + "void main() { alligator_main(vulkan_job(vulkan_job_index())); }\n");
    const std::array<std::string, 2> names{"public-list", "job-table"};
    id<MTLComputePipelineState> pipelines[2]{
        prepare(device, public_glsl, names[0], artifacts),
        prepare(device, jobs_glsl, names[1], artifacts)};
    constexpr std::array<size_t, 8> lengths{512, 128, 128, 512, 512, 128, 64, 64};
    id<MTLBuffer> buffers[8];
    id<MTLResource> resources[8];
    for (size_t index = 0; index < lengths.size(); ++index) {
        buffers[index] = [device newBufferWithLength:lengths[index] options:MTLResourceStorageModeShared];
        require(buffers[index] != nil && buffers[index].gpuAddress != 0, "Native buffer allocation failed");
        std::fill_n(static_cast<uint32_t*>(buffers[index].contents), lengths[index] / 4, 0u);
        resources[index] = buffers[index];
    }
    auto* directory = static_cast<uint64_t*>(buffers[0].contents);
    directory[3] = buffers[1].gpuAddress;
    directory[37] = buffers[2].gpuAddress;
    auto* first_table = static_cast<GPUBuf*>(buffers[1].contents);
    auto* second_table = static_cast<GPUBuf*>(buffers[2].contents);
    first_table[1].set(buffers[3].gpuAddress, 1, 1);
    first_table[2].set(buffers[5].gpuAddress, 1, 0);
    first_table[3].set(buffers[6].gpuAddress, 1, 0);
    first_table[4].set(buffers[3].gpuAddress, 1, 3);
    second_table[1].set(buffers[4].gpuAddress, 1, 1);
    second_table[2].set(buffers[5].gpuAddress, 1, 1);
    second_table[3].set(buffers[4].gpuAddress, 1, 5);
    auto* parameters = static_cast<uint32_t*>(buffers[5].contents);
    parameters[0] = identifier(1, 3);
    parameters[1] = identifier(1, 37);
    parameters[2] = 100;
    parameters[16] = identifier(4, 3);
    parameters[17] = identifier(3, 37);
    parameters[18] = 101;
    *static_cast<uint32_t*>(buffers[7].contents) = identifier(3, 3);
    const PushBlock push{buffers[7].gpuAddress, buffers[0].gpuAddress};
    id<MTLCommandQueue> queue = [device newCommandQueue];
    require(queue != nil, "Native command queue allocation failed");
    auto* output = static_cast<uint32_t*>(buffers[4].contents);
    for (size_t format = 0; format < names.size(); ++format) {
        auto* list = static_cast<uint32_t*>(buffers[6].contents);
        std::fill_n(list, 16, 0u);
        list[0] = identifier(2, 3);
        list[format == 0 ? 1 : 8] = identifier(2, 37);
        for (uint32_t round = 0; round < 8; ++round) {
            parameters[3] = round;
            parameters[19] = round;
            auto* payload = static_cast<unsigned char*>(buffers[3].contents);
            *reinterpret_cast<simd_float4*>(payload + 64) = simd_float4{1, 2, 3, 4} + float(round);
            *reinterpret_cast<simd_float4*>(payload + 192) = simd_float4{10, 20, 30, 40} + float(round);
            std::fill_n(output, lengths[4] / 4, UINT32_C(0xdeadbeef));
            id<MTLCommandBuffer> command = [queue commandBuffer];
            command.label = @(names[format].c_str());
            id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
            [encoder setComputePipelineState:pipelines[format]];
            [encoder setBytes:&push length:sizeof(push) atIndex:0];
            [encoder useResources:resources count:8 usage:MTLResourceUsageRead | MTLResourceUsageWrite];
            [encoder dispatchThreadgroups:(format == 0 ? MTLSizeMake(1, 2, 1) : MTLSizeMake(1, 1, 2))
                threadsPerThreadgroup:MTLSizeMake(16, 4, 1)];
            [encoder endEncoding];
            [command commit];
            [command waitUntilCompleted];
            require(command.status == MTLCommandBufferStatusCompleted,
                names[format] + " command failed: "
                + (command.error ? std::string(command.error.description.UTF8String) : "no diagnostic"));
            for (size_t row = 0; row < 2; ++row) {
                const size_t start = row == 0 ? 16 : 80;
                const float* values = reinterpret_cast<const float*>(output + start);
                const float factor = row == 0 ? 1.0f : 10.0f;
                for (size_t lane = 0; lane < 4; ++lane)
                    require(values[lane] == factor * float(lane + 1) * 2.0f + float(round) * 3.0f,
                        names[format] + " vector output differs");
                require(output[start + 4] == 64 && output[start + 5] == 64
                    && output[start + 6] == 1 && output[start + 7] == 2
                    && *reinterpret_cast<const uint64_t*>(output + start + 8)
                        == buffers[3].gpuAddress + (row == 0 ? 64 : 192)
                    && output[start + 10] == UINT32_C(0xdeadbeef)
                    && output[start + 11] == UINT32_C(0xdeadbeef)
                    && output[start + 12] == 100 + row && output[start + 13] == round
                    && output[start + 14] == 64 && output[start + 15] == 2,
                    names[format] + " nested IDs, granules, count, address, or guard differed");
                LOG_INFO_STREAM << names[format] << " round=" << round << " row=" << row
                    << " vector={" << values[0] << ',' << values[1] << ',' << values[2] << ',' << values[3]
                    << "} inputBytes=" << output[start + 4] << " parameterBytes=" << output[start + 5]
                    << " null=" << output[start + 6] << " count=" << output[start + 7]
                    << " inputAddress=" << *reinterpret_cast<const uint64_t*>(output + start + 8)
                    << " tag=" << output[start + 12] << " outputBytes=" << output[start + 14];
            }
            for (size_t word = 0; word < lengths[4] / 4; ++word) {
                if ((word >= 16 && word < 32) || (word >= 80 && word < 96)) continue;
                require(output[word] == UINT32_C(0xdeadbeef), "GPU write escaped a represented Slice");
            }
        }
        LOG_INFO_STREAM << names[format] << " passed all repeated output and untouched-region checks";
    }
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Records complete translation artifacts and runs the native Metal feasibility gate.
 */
int main(int count, char** arguments) {
    @autoreleasepool {
        try {
            const std::filesystem::path artifacts = count == 2 ? arguments[1] : "metal-glsl-artifacts";
            std::filesystem::create_directories(artifacts);
            run(artifacts);
            LOG_INFO_STREAM << "METAL_GLSL_GATE_PASSED: production public-list and job-table ABI executed";
            return 0;
        } catch (const std::exception& error) {
            LOG_ERROR_STREAM << "METAL_GLSL_GATE_FAILED: " << error.what();
            return 1;
        }
    }
}
