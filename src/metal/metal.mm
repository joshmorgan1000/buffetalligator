/** --------------------------------------------------------------------------------------------------------- Native Metal Shaders
 * @file metal.mm
 * @brief Prepares native or translated Metal programs for the shared asynchronous Shader engine.
 */
#include <metal/metal.hpp>
#include <metal/context.hpp>
#include <metal/prelude.hpp>
#include <spirv_msl.hpp>
#include <algorithm>
#include <limits>

namespace buffetalligator {
namespace {
/** --------------------------------------------------------------------------------------------------------- Metal Push
 * @brief Matches the two physical-address fields shared by every shader backend.
 */
struct MetalPush { uint64_t table; uint64_t directory; };
static_assert(sizeof(MetalPush) == 16 && offsetof(MetalPush, directory) == 8);
/** --------------------------------------------------------------------------------------------------------- Metal Stage
 * @brief Retains only the immutable dispatch shape of a prepared stage.
 */
struct MetalStage { uint32_t width; uint32_t height; bool per_request; };
/** --------------------------------------------------------------------------------------------------------- Metal Slot
 * @brief Owns one stable indirect parameter table and its current command-buffer completion.
 */
struct MetalSlot {
    std::unique_ptr<MetalBuffer> storage;
    id<MTLCommandBuffer> command = nil;
};
/** --------------------------------------------------------------------------------------------------------- Metal Program
 * @brief Owns pipelines and native slots until their common admission owners have retired.
 */
struct MetalProgram {
    std::string name;
    NSString* label = nil;
    std::vector<id<MTLComputePipelineState>> pipelines;
    std::vector<MetalStage> stages;
    std::vector<std::unique_ptr<MetalSlot>> slots;
    Slice references;
    /** ------------------------------------------------------------------------------------------- Destroy
     * @brief Releases native resources before the shared engine releases its Slice parameter tables.
     */
    static void destroy(ShaderProgram& program) noexcept {
        delete static_cast<MetalProgram*>(program.native);
        program.native = nullptr;
    }
    /** ------------------------------------------------------------------------------------------- Submit
     * @brief Encodes prepared pipelines with declared hazards and commits one retained command buffer.
     */
    static void submit(ShaderProgram& program, ShaderSlot& slot,
        uint32_t width, uint32_t height, uint32_t depth
    ) {
        MetalContext& context = metal_context();
        auto& prepared = *static_cast<MetalProgram*>(program.native);
        auto& native = *static_cast<MetalSlot*>(slot.native);
        @autoreleasepool {
            id<MTLCommandBuffer> command = [context.queue commandBuffer];
            if (!command) ALLIGATOR_GPU_THROW("Metal command buffer allocation failed");
            command.label = prepared.label;
            id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
            if (!encoder) ALLIGATOR_GPU_THROW("Metal compute encoder allocation failed");
            const MetalPush push{MetalBuffer::device_address(native.storage.get()),
                Alligator::gpu_directory_address()};
            [encoder setBytes:&push length:sizeof(push) atIndex:0];
            {
                std::lock_guard lock(context.allocations);
                if (@available(macOS 15.0, *)) {
                    if (context.residency && context.dirty) {
                        [context.residency commit];
                        context.dirty = false;
                    }
                }
                [encoder useResources:context.resources.data() count:context.resources.size()
                    usage:MTLResourceUsageRead | MTLResourceUsageWrite];
                if (prepared.stages.empty()) {
                    [encoder setComputePipelineState:prepared.pipelines[0]];
                    [encoder dispatchThreadgroups:MTLSizeMake(width, height, depth)
                        threadsPerThreadgroup:MTLSizeMake(16, 4, 1)];
                } else {
                    for (size_t index = 0; index < prepared.stages.size(); ++index) {
                        const MetalStage& stage = prepared.stages[index];
                        [encoder setComputePipelineState:prepared.pipelines[index]];
                        [encoder dispatchThreadgroups:MTLSizeMake(stage.width, stage.height,
                            stage.per_request ? depth : 1) threadsPerThreadgroup:MTLSizeMake(16, 4, 1)];
                        if (index + 1 < prepared.stages.size())
                            [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
                    }
                }
                [encoder endEncoding];
                native.command = command;
                [command commit];
            }
        }
    }
    /** ------------------------------------------------------------------------------------------- Wait
     * @brief Retires the command on a Kitchen worker and reports terminal native execution errors.
     */
    static void wait(ShaderProgram& program, ShaderSlot& slot) {
        auto& native = *static_cast<MetalSlot*>(slot.native);
        @autoreleasepool {
            id<MTLCommandBuffer> command = native.command;
            [command waitUntilCompleted];
            native.command = nil;
            if (command.status != MTLCommandBufferStatusCompleted) {
                const auto& prepared = *static_cast<MetalProgram*>(program.native);
                ALLIGATOR_GPU_THROW("Metal dispatch [" + prepared.name + "]: "
                    + (command.error ? std::string(command.error.description.UTF8String)
                        : "command did not complete"));
            }
        }
    }
};
const ShaderBackendOps METAL_OPERATIONS{MetalProgram::destroy, MetalProgram::submit, MetalProgram::wait};
} // namespace
/** --------------------------------------------------------------------------------------------------------- Prepare Metal
 * @brief Compiles the selected source and prepares independent retained parameter slots.
 */
std::unique_ptr<ShaderProgram> metal_prepare(const ShaderPrepareInfo& info) {
    MetalContext& context = metal_context();
    auto program = std::make_unique<ShaderProgram>();
    auto native = std::make_unique<MetalProgram>();
    native->name = info.name;
    native->label = @(native->name.c_str());
    if (info.references) native->references = info.references->slice();
    program->native = native.release();
    program->ops = &METAL_OPERATIONS;
    auto& prepared = *static_cast<MetalProgram*>(program->native);
    const size_t record_bytes = info.format == ShaderFormat::Jobs ? 32 : sizeof(Slice);
    const size_t grid_limit = info.format == ShaderFormat::Slices ? UINT32_MAX / 4 : UINT32_MAX;
    program->capacity = std::min({info.capacity, grid_limit, context.maximum_length / record_bytes});
    program->max_workgroups_x = UINT32_MAX / 16;
    if (!program->capacity) ALLIGATOR_GPU_THROW("Metal preparation requires nonempty representable capacity");
    const size_t maximum_storage = std::min(context.maximum_length, size_t(uint64_t{UINT32_MAX} << 6));
    if (info.stages.size() > (maximum_storage - 32) / 16)
        ALLIGATOR_GPU_THROW("Metal stage metadata exceeds its representable buffer limit");
    const size_t storage_bytes = (32 + info.stages.size() * 16 + 63) & ~size_t{63};
    const size_t table_bytes = info.format == ShaderFormat::References ? 0
        : ((program->capacity * record_bytes + 63) & ~size_t{63});
    const size_t working_set = context.device.recommendedMaxWorkingSetSize;
    const size_t slot_count = working_set == 0 ? context.slots
        : std::min(context.slots, working_set / (storage_bytes + table_bytes));
    if (!slot_count) ALLIGATOR_GPU_THROW("Metal working-set budget cannot hold one prepared slot");
    const size_t pipeline_count = info.stages.empty() ? 1 : info.stages.size();
    prepared.pipelines.reserve(pipeline_count);
    prepared.stages.reserve(info.stages.size());
    for (size_t index = 0; index < pipeline_count; ++index) {
        LOG_INFO_STREAM << "Preparing Metal shader " << prepared.name
            << " pipeline " << index + 1 << '/' << pipeline_count;
        std::string source;
        if (!info.source.metal.empty()) {
            if (info.format == ShaderFormat::References)
                ALLIGATOR_GPU_THROW("Native Metal reference programs require a GLSL reference body");
            source = METAL_SHADER_CORE;
            source.append(info.source.metal);
            source.append(METAL_SHADER_ENTRY);
            source.append(info.format == ShaderFormat::Jobs
                ? "program.alligator_main(program.vulkan_job(program.vulkan_job_index()));\n}\n"
                : "program.alligator_main(program.vulkan_slice(program.vulkan_index()));\n}\n");
        } else {
            std::vector<uint32_t> words;
            if (!info.words.empty()) words.assign(info.words.begin(), info.words.end());
            else {
                ShaderPrepareInfo stage = info;
                if (!info.stages.empty()) stage.source.glsl = info.stages[index].glsl;
                words = shader_compile_glsl(shader_glsl_source(stage), true, info.name);
            }
            if (index == 0) program->spirv = words;
            spirv_cross::CompilerMSL translator(words);
            auto options = translator.get_msl_options();
            options.platform = spirv_cross::CompilerMSL::Options::macOS;
            options.set_msl_version(3, 0);
            translator.set_msl_options(options);
            const auto resources = translator.get_shader_resources();
            if (resources.push_constant_buffers.size() > 1)
                ALLIGATOR_GPU_THROW("Metal translation exposed multiple physical-address push blocks");
            if (!resources.push_constant_buffers.empty()) {
                const auto& push = translator.get_type(resources.push_constant_buffers[0].base_type_id);
                if (translator.get_declared_struct_size(push) != 16
                    || translator.type_struct_member_offset(push, 0) != 0
                    || translator.type_struct_member_offset(push, 1) != 8)
                    ALLIGATOR_GPU_THROW("Metal translation changed the 16-byte push-block layout");
            }
            spirv_cross::MSLResourceBinding binding;
            binding.stage = spv::ExecutionModelGLCompute;
            binding.desc_set = spirv_cross::ResourceBindingPushConstantDescriptorSet;
            binding.binding = spirv_cross::ResourceBindingPushConstantBinding;
            binding.msl_buffer = 0;
            translator.add_msl_resource_binding(binding);
            translator.rename_entry_point("main", "alligator_entry", spv::ExecutionModelGLCompute);
            source = translator.compile();
        }
        @autoreleasepool {
            MTLCompileOptions* options = [MTLCompileOptions new];
            options.languageVersion = MTLLanguageVersion3_0;
            NSError* error = nil;
            id<MTLLibrary> library = [context.device newLibraryWithSource:@(source.c_str())
                options:options error:&error];
            if (!library) ALLIGATOR_GPU_THROW("Metal source [" + prepared.name + "]: "
                + (error ? std::string(error.description.UTF8String) : "compilation failed"));
            if (error) LOG_WARN_STREAM << "Metal shader " << prepared.name << ": "
                << error.description.UTF8String;
            id<MTLFunction> function = [library newFunctionWithName:@"alligator_entry"];
            if (!function) ALLIGATOR_GPU_THROW("Metal shader has no alligator_entry function");
            error = nil;
            id<MTLComputePipelineState> pipeline =
                [context.device newComputePipelineStateWithFunction:function error:&error];
            if (!pipeline) ALLIGATOR_GPU_THROW("Metal pipeline [" + prepared.name + "]: "
                + (error ? std::string(error.description.UTF8String) : "creation failed"));
            if (pipeline.maxTotalThreadsPerThreadgroup < 64
                || pipeline.staticThreadgroupMemoryLength > context.device.maxThreadgroupMemoryLength)
                ALLIGATOR_GPU_THROW("Metal pipeline cannot execute the 16x4x1 threadgroup");
            prepared.pipelines.push_back(pipeline);
            LOG_INFO_STREAM << "Metal shader " << prepared.name << ": execution width="
                << pipeline.threadExecutionWidth << ", max threads="
                << pipeline.maxTotalThreadsPerThreadgroup << ", shared bytes="
                << pipeline.staticThreadgroupMemoryLength;
        }
        if (!info.stages.empty()) {
            const auto& stage = info.stages[index];
            if (stage.workgroups_x > program->max_workgroups_x || stage.workgroups_y > UINT32_MAX / 4)
                ALLIGATOR_GPU_THROW("Metal stage exceeds its uint3 invocation-coordinate representation");
            prepared.stages.push_back({stage.workgroups_x, stage.workgroups_y, stage.per_request});
        }
    }
    prepared.slots.reserve(slot_count);
    program->slots.reserve(slot_count);
    for (size_t index = 0; index < slot_count; ++index) {
        auto slot = std::make_unique<ShaderSlot>();
        auto owned = std::make_unique<MetalSlot>();
        owned->storage = std::make_unique<MetalBuffer>(storage_bytes);
        slot->mapped = static_cast<uint8_t*>(owned->storage->raw());
        if (info.format == ShaderFormat::References) {
            const GPUBuf& record = *Alligator::gpubuf_for(*info.references);
            *reinterpret_cast<uint64_t*>(slot->mapped) = record.address + (uint64_t(record.offset) << 6);
            reinterpret_cast<uint32_t*>(slot->mapped)[2] = uint32_t(info.references->size<uint32_t>() - 1);
            reinterpret_cast<uint32_t*>(slot->mapped)[7] = info.resources;
        } else {
            slot->parameters = Slice(program->capacity * record_bytes, context.descriptor);
            *reinterpret_cast<uint32_t*>(slot->mapped) = slot->parameters.id();
        }
        slot->native = owned.get();
        prepared.slots.push_back(std::move(owned));
        program->slots.push_back(std::move(slot));
    }
    LOG_INFO_STREAM << "Metal shader " << prepared.name << " prepared " << slot_count
        << " submission slots with capacity " << program->capacity;
    return program;
}
} // namespace buffetalligator
