/** --------------------------------------------------------------------------------------------------------- Vulkan Implementation
 * @file vulkan.cpp
 * @brief Implements Vulkan slabs, GLSL compilation, and the public prepared Shader interface.
 */
#include <logging.hpp>
#include <alligator.hpp>
#include <vulkan/vulkan.hpp>
#include <shaderc/shaderc.hpp>
#include <vulkan/vulkanhelpers.hpp>
#include <vulkan/vulkancontext.hpp>
#include <vulkan/vulkanbuffer.hpp>
#include <vulkan/vulkankernel.hpp>
#include <vulkan/vulkanglsl.hpp>
#include <memory/buffetmanager.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace buffetalligator {
namespace {
/** --------------------------------------------------------------------------------------------------------- Kernel Push
 * @struct KernelPush
 * @brief The host mirror of the kernel core's push block (vulkanglsl.hpp): the engine's table
 * address and the global GPUBufRef pool's device address, 16 bytes.
 */
struct KernelPush {
    uint64_t table;  ///< Job table (megakernel) or parameters list (public Shader)
    uint64_t pool;   ///< The global GPUBufRef table's device address
};
static_assert(sizeof(KernelPush) == 16, "KernelPush must mirror the GLSL push block");
/** --------------------------------------------------------------------------------------------------------- SliceMirror
 * @struct SliceMirror
 * @brief The host copy of the GLSL `Slice` descriptor (vulkanglsl.hpp): address, size, offset in bytes.
 */
struct alignas(16) SliceMirror {
    uint64_t device_address;  ///< Absolute device address of the claim's slab
    uint32_t size;            ///< Claim size in bytes
    uint32_t offset;          ///< Claim offset within the addressed buffer
};
static_assert(sizeof(SliceMirror) == 16, "SliceMirror must match the GLSL Slice descriptor.");
/// @brief Slices one prepared Shader binds per round; longer lists dispatch in rounds.
constexpr size_t PUBLIC_LIST_CAPACITY = 1024;
/** --------------------------------------------------------------------------------------------------------- Public GLSL Tail
 * @brief Binds the list descriptor's address and hands workgroup Y its own slice.
 */
inline constexpr std::string_view PUBLIC_GLSL_TAIL = R"glsl(
Slice vulkan_list() { return slice_read(vulkan_push.vulkan_table_address); }
uint vulkan_count() { return slice_size(vulkan_list()) / 16u; }
Slice vulkan_slice(uint index) { return slice_read(slice_address(vulkan_list()) + uint64_t(index) * 16ul); }
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
        "\nvoid main() { vulkan_main(vulkan_slice(vulkan_index())); }\n";
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
} // namespace
/** --------------------------------------------------------------------------------------------------------- Compile Vulkan GLSL
 * @brief Compiles GLSL source code into Vulkan SPIR-V binary format.
 * @param source GLSL source code as a string view.
 * @param float16 Whether to enable 16-bit floating point support.
 * @param name Diagnostic name for error reporting.
 * @return A vector of 32-bit words representing the compiled SPIR-V binary.
 */
std::vector<uint32_t> compile_vulkan_glsl(
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
        ALLIGATOR_GPU_THROW("Vulkan shader compile failed [" + diagnostic_name + "]: "
            + result.GetErrorMessage());
    }
    return std::vector<uint32_t>(result.cbegin(), result.cend());
}
/** --------------------------------------------------------------------------------------------------------- VulkanBuffer Constructor
 * @brief Create, allocate (DEVICE_LOCAL|HOST_VISIBLE preferred, else HOST_VISIBLE), bind, map,
 * zero, and query the device address.
 * @param size_bytes Buffer size in bytes.
 */
VulkanBuffer::VulkanBuffer(size_t size_bytes)
: VulkanBuffer(size_bytes, VulkanContext::instance().buffer_memory_type_index_) {}
/** --------------------------------------------------------------------------------------------------------- VulkanBuffer Constructor (typed)
 * @brief The same five-call allocation against an explicit memory type index.
 * @param size_bytes Buffer size in bytes.
 * @param memory_type_index The memory type to allocate from.
 */
VulkanBuffer::VulkanBuffer(size_t size_bytes, uint32_t memory_type_index)
: size_(size_bytes) {
    if (!VulkanContext::device_present()) {
        host_ = std::malloc(size_bytes);
        std::memset(host_, 0, size_bytes);
        address_ = 0;
        return;
    }
    buffer_ = VulkanContext::instance().device_.createBuffer(
        VulkanContext::buffer_create_info(size_bytes, VulkanContext::instance().buffer_usage_));
    const vk::MemoryRequirements requirements = VulkanContext::instance().device_.getBufferMemoryRequirements(buffer_);
    vk::MemoryAllocateInfo allocate_info(requirements.size, memory_type_index);
    allocate_info.pNext = &VulkanContext::instance().allocate_flags_;
    memory_ = VulkanContext::instance().device_.allocateMemory(allocate_info);
    VulkanContext::instance().device_.bindBufferMemory(buffer_, memory_, 0);
    host_ = VulkanContext::instance().device_.mapMemory(memory_, 0, VK_WHOLE_SIZE);
    address_ = VulkanContext::instance().device_.getBufferAddress(vk::BufferDeviceAddressInfo(buffer_));
    std::memset(host_, 0, size_);
}
/** --------------------------------------------------------------------------------------------------------- VulkanBuffer Destructor
 * @brief Unmap and free through the owning context, unless that context has already been torn down.
 */
VulkanBuffer::~VulkanBuffer() {
    if (!memory_) {
        std::free(host_);
        return;
    }
    if (!VulkanContext::alive_.load(std::memory_order_acquire)) {
        return;
    }
    VulkanContext::instance().device_.unmapMemory(memory_);
    VulkanContext::instance().device_.destroyBuffer(buffer_);
    VulkanContext::instance().device_.freeMemory(memory_);
}
/** --------------------------------------------------------------------------------------------------------- GPU::exists
 * @brief Checks if a Vulkan compute device is present.
 * @return True if a Vulkan compute device is available, false otherwise.
 */
bool GPU::exists() {
    return VulkanContext::device_present();
}
/** --------------------------------------------------------------------------------------------------------- GPU::unified_memory
 * @brief Checks if the Vulkan compute device uses unified memory.
 * @return True if the device has unified memory, false otherwise.
 */
bool GPU::unified_memory() {
    return VulkanContext::device_present() && VulkanContext::device_unified();
}
/** --------------------------------------------------------------------------------------------------------- GPU::device_name
 * @brief Retrieves the name of the Vulkan compute device.
 * @return The device name as a string.
 */
std::string GPU::device_name() {
    return VulkanKernel::device_name();
}
/** --------------------------------------------------------------------------------------------------------- GPU::compile_glsl
 * @brief Compiles GLSL source code into a Vulkan SPIR-V binary.
 * @param body The body of the shader function to be injected into the final source.
 * @return A Slice containing the compiled SPIR-V words.
 */
Slice GPU::compile_glsl(std::string_view body) {
    if (!VulkanContext::device_present()) {
        GPU_THROW("GPU::compile_glsl: no Vulkan compute device is present.");
    }
    if (!VulkanContext::device_properties().supports_int64) {
        GPU_THROW("GPU::compile_glsl: the prelude addresses memory with uint64_t, "
            "but the device lacks shaderInt64.");
    }
    const std::string assembled = kernel_shader_source(body);
    std::vector<uint32_t> spirv;
    try {
        spirv = compile_vulkan_glsl(
            assembled,
            VulkanContext::device_properties().supports_float16,
            "vulkan_gpu_compile_glsl");
    } catch (const std::exception& error) {
        GPU_THROW(std::string("GPU::compile_glsl: ") + error.what());
    }
    Slice words(4 * spirv.size());
    std::memcpy(words.raw(), spirv.data(), words.size_bytes());
    return words;
}
/** --------------------------------------------------------------------------------------------------------- ShaderState
 * @class ShaderState
 * @brief Owns one pipeline, persistent parameter block, immutable command buffer, and fence.
 */
struct ShaderState::Impl {
    enum class Format { Slices, Jobs, References };
    Format format;
    std::vector<uint32_t> words;
    std::string name;
    size_t limit;
    uint32_t reference_workgroups_x = 1;
    vk::Pipeline pipeline{};
    std::vector<std::vector<uint32_t>> stage_words;
    std::vector<KernelGpuStage> stages;
    std::vector<vk::Pipeline> stage_pipelines;
    uint32_t resources = 0;

    vk::PipelineLayout layout{};
    vk::ShaderModule module{};
    vk::Buffer buffer{};
    vk::DeviceMemory memory{};
    uint8_t* mapped = nullptr;
    uint64_t address = 0;
    std::vector<vk::CommandPool> command_pools;
    std::vector<vk::CommandBuffer> commands;
    vk::Fence fence{};

    Impl(std::vector<uint32_t> code, std::string_view label, Format fmt,
         size_t capacity, const Slice* refs = nullptr)
        : format(fmt), words(std::move(code)), name(label), limit(capacity) {
        try { prepare(refs); } catch (...) { release(); throw; }
    }
    Impl(std::vector<std::vector<uint32_t>> code, std::span<const KernelGpuStage> sequence,
         std::string_view label, const Slice& refs, uint32_t resource_ref)
        : format(Format::References), words(code.front()), name(label), limit(refs.size<uint32_t>()),
          stage_words(std::move(code)), stages(sequence.begin(), sequence.end()), resources(resource_ref) {
        try { prepare(&refs); } catch (...) { release(); throw; }
    }
    ~Impl() { release(); }

    void release() {
        const auto device = VulkanContext::device();
        for (const auto pool : command_pools) device.destroyCommandPool(pool);
        if (fence) device.destroyFence(fence);
        if (pipeline) device.destroyPipeline(pipeline);
        for (auto entry : stage_pipelines) device.destroyPipeline(entry);
        if (layout) device.destroyPipelineLayout(layout);
        if (module) device.destroyShaderModule(module);
        if (mapped) device.unmapMemory(memory);
        if (buffer) device.destroyBuffer(buffer);
        if (memory) device.freeMemory(memory);
    }
    void prepare(const Slice* refs) {
        const auto device = VulkanContext::device();
        const auto& props = VulkanContext::device_properties();
        if (props.max_workgroup_size[0] < 16 || props.max_workgroup_size[1] < 4
            || props.max_workgroup_invocations < 64)
            GPU_THROW("Kernel: device cannot execute the (16,4,1) local shape");
        limit = std::min(limit, size_t(props.max_workgroup_count[format == Format::Slices ? 1 : 2]));
        module = device.createShaderModule(vk::ShaderModuleCreateInfo({}, words.size() * 4, words.data()));
        const vk::PushConstantRange range(vk::ShaderStageFlagBits::eCompute, 0, sizeof(KernelPush));
        layout = device.createPipelineLayout(vk::PipelineLayoutCreateInfo({}, 0, nullptr, 1, &range));
        const vk::PipelineShaderStageCreateInfo stage({}, vk::ShaderStageFlagBits::eCompute, module, "main");
        const auto created = device.createComputePipeline(VulkanContext::pipeline_cache(),
            vk::ComputePipelineCreateInfo({}, stage, layout));
        if (created.result != vk::Result::eSuccess) GPU_THROW("Kernel pipeline: " + vk::to_string(created.result));
        pipeline = created.value;
        device.destroyShaderModule(module); module = nullptr;
        for (size_t index = 1; index < stage_words.size(); ++index) {
            const auto& code = stage_words[index];
            module = device.createShaderModule(vk::ShaderModuleCreateInfo({}, code.size() * 4, code.data()));
            const vk::PipelineShaderStageCreateInfo pass({}, vk::ShaderStageFlagBits::eCompute, module, "main");
            const auto built = device.createComputePipeline(VulkanContext::pipeline_cache(),
                vk::ComputePipelineCreateInfo({}, pass, layout));
            if (built.result != vk::Result::eSuccess) GPU_THROW("Kernel stage pipeline: " + vk::to_string(built.result));
            stage_pipelines.push_back(built.value);
            device.destroyShaderModule(module); module = nullptr;
        }
        const size_t bytes = 32 + (format == Format::References ? stages.size() * 16
            : limit * (format == Format::Jobs ? 32 : 16));

        buffer = device.createBuffer(VulkanContext::buffer_create_info(bytes,
            vk::BufferUsageFlagBits::eShaderDeviceAddress | vk::BufferUsageFlagBits::eStorageBuffer
            | vk::BufferUsageFlagBits::eIndirectBuffer));
        const auto requirements = device.getBufferMemoryRequirements(buffer);
        const uint32_t type = VulkanContext::buffer_memory_type_index();
        if (!(requirements.memoryTypeBits & (1u << type))) GPU_THROW("Kernel: incompatible coherent memory type");
        vk::MemoryAllocateInfo allocation(requirements.size, type);
        allocation.pNext = &VulkanContext::allocate_flags();
        memory = device.allocateMemory(allocation);
        device.bindBufferMemory(buffer, memory, 0);
        mapped = static_cast<uint8_t*>(device.mapMemory(memory, 0, VK_WHOLE_SIZE));
        std::memset(mapped, 0, bytes);
        address = device.getBufferAddress(vk::BufferDeviceAddressInfo(buffer));
        if (format == Format::References) {
            *reinterpret_cast<uint64_t*>(mapped) = VulkanKernel::device_address(*refs);
            reinterpret_cast<uint32_t*>(mapped)[2] = uint32_t(refs->size<uint32_t>() - 1);
            reinterpret_cast<uint32_t*>(mapped)[7] = resources;
        } else {
            *reinterpret_cast<SliceMirror*>(mapped) = SliceMirror{address, 0, 32};
        }
        command_pools.reserve(VulkanContext::compute_families().size());
        commands.reserve(VulkanContext::compute_families().size());
        fence = device.createFence({});
        for (const uint32_t family : VulkanContext::compute_families()) {
            const auto command_pool = device.createCommandPool(vk::CommandPoolCreateInfo({}, family));
            command_pools.push_back(command_pool);
            const auto command = device.allocateCommandBuffers(vk::CommandBufferAllocateInfo(
                command_pool, vk::CommandBufferLevel::ePrimary, 1))[0];
            commands.push_back(command);
            const KernelPush push{address, GlobalPool::instance().gpu_pool_address()};
            static_cast<void>(command.begin(vk::CommandBufferBeginInfo{}));
            command.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            command.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
            if (stages.empty()) {
                command.dispatchIndirect(buffer, 16);
            } else {
                for (size_t index = 0; index < stages.size(); ++index) {
                    command.bindPipeline(vk::PipelineBindPoint::eCompute,
                        index == 0 ? pipeline : stage_pipelines[index - 1]);
                    command.dispatchIndirect(buffer, 32 + index * 16);
                    const vk::MemoryBarrier dependency(vk::AccessFlagBits::eShaderWrite,
                        vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite
                        | vk::AccessFlagBits::eIndirectCommandRead);
                    command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                        vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eDrawIndirect,
                        {}, 1, &dependency, 0, nullptr, 0, nullptr);
                }
            }

            // Coherent allocation removes flush/invalidate, not the device-to-host dependency.
            const vk::MemoryBarrier readback(vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eHostRead);
            command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eHost,
                {}, 1, &readback, 0, nullptr, 0, nullptr);
            command.end();
        }
    }
    void submit(uint32_t x, uint32_t y, uint32_t z) {
        *reinterpret_cast<vk::DispatchIndirectCommand*>(mapped + 16) = vk::DispatchIndirectCommand(x, y, z);
        VulkanContext::submit_command_buffer(
            commands[VulkanContext::submission_family_slot()], fence, nullptr, nullptr);
        const auto result = VulkanContext::device().waitForFences(fence, VK_TRUE, UINT64_MAX);
        if (result != vk::Result::eSuccess) GPU_THROW("Kernel fence wait: " + vk::to_string(result));
    }
};
namespace {
void require_kernel_device() {
    if (!VulkanContext::device_present()) GPU_THROW("Kernel: no Vulkan compute device");
    if (!VulkanContext::device_properties().supports_int64) GPU_THROW("Kernel: shaderInt64 is required");
}
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
    vulkan_main(gpu_slice(slice_load_u32(invocation, 0u)), gpu_slice(slice_load_u32(invocation, 1u)));
}
)glsl");
    return source;
}
} // namespace
ShaderState::ShaderState(std::string_view source, std::string_view name, const Slice* references, uint32_t workgroups_x) {
    require_kernel_device();
    if (!workgroups_x || workgroups_x > VulkanContext::device_properties().max_workgroup_count[0])
        GPU_THROW("Kernel: workgroups_x is outside the device limit");
    auto words = compile_vulkan_glsl(references ? reference_shader_source(source) : public_shader_source(source),
        VulkanContext::device_properties().supports_float16, name);
    impl_ = std::make_unique<Impl>(std::move(words), name,
        references ? Impl::Format::References : Impl::Format::Slices,
        references ? references->size<uint32_t>() : PUBLIC_LIST_CAPACITY, references);
    impl_->reference_workgroups_x = workgroups_x;
}
ShaderState::ShaderState(std::span<const KernelGpuStage> stages, std::string_view name,
                        const Slice& references, uint32_t resources) {
    require_kernel_device();
    const auto& props = VulkanContext::device_properties();
    std::vector<std::vector<uint32_t>> code;
    for (const auto& stage : stages) {
        if (!stage.workgroups_x || stage.workgroups_x > props.max_workgroup_count[0]
            || stage.workgroups_y > props.max_workgroup_count[1])
            GPU_THROW("Kernel stage dispatch exceeds the device shape");
        code.push_back(compile_vulkan_glsl(reference_shader_source(stage.glsl), props.supports_float16, name));
    }
    impl_ = std::make_unique<Impl>(std::move(code), stages, name, references, resources);
}
ShaderState::ShaderState(const uint32_t* words, size_t count, std::string_view name, size_t max_jobs) {

    require_kernel_device();
    impl_ = std::make_unique<Impl>(std::vector<uint32_t>(words, words + count), name, Impl::Format::Jobs, max_jobs);
}
ShaderState::~ShaderState() = default;
const std::vector<uint32_t>& ShaderState::spirv() const { return impl_->words; }
const std::string& ShaderState::name() const { return impl_->name; }
size_t ShaderState::capacity() const { return impl_->limit; }
void ShaderState::dispatch(const GPUSlice* streams, size_t count, uint32_t workgroups) const {
    workgroups = std::min(workgroups, VulkanContext::device_properties().max_workgroup_count[0]);
    for (size_t first = 0; first < count; first += impl_->limit) {
        const size_t n = std::min(impl_->limit, count - first);
        auto* entries = reinterpret_cast<SliceMirror*>(impl_->mapped + 32);
        for (size_t i = 0; i < n; ++i) {
            entries[i] = SliceMirror{VulkanKernel::device_address(streams[first + i]),
                uint32_t(streams[first + i].size_bytes()), 0};
        }
        reinterpret_cast<SliceMirror*>(impl_->mapped)->size = uint32_t(n * 16);
        impl_->submit(workgroups, uint32_t(n), 1);
    }
}
void ShaderState::write_job(size_t slot, const Slice& input, const void* host_handle) {
    uint8_t* record = impl_->mapped + 32 + slot * 32;
    *reinterpret_cast<SliceMirror*>(record) = SliceMirror{VulkanKernel::device_address(input), uint32_t(input.size_bytes()), 0};
    std::memcpy(record + 16, host_handle, 16);
}
void ShaderState::dispatch(size_t jobs) {
    reinterpret_cast<SliceMirror*>(impl_->mapped)->size = uint32_t(jobs * 32);
    impl_->submit(1, 1, uint32_t(jobs));
}
void ShaderState::dispatch_references(uint32_t first, uint32_t count) {
    reinterpret_cast<uint32_t*>(impl_->mapped)[3] = first;
    for (size_t index = 0; index < impl_->stages.size(); ++index) {
        const auto& stage = impl_->stages[index];
        *reinterpret_cast<vk::DispatchIndirectCommand*>(impl_->mapped + 32 + index * 16) =
            vk::DispatchIndirectCommand(stage.workgroups_x, stage.workgroups_y, stage.per_request ? count : 1);
    }
    impl_->submit(impl_->reference_workgroups_x, 1, count);

}
/** --------------------------------------------------------------------------------------------------------- Shader::Shader
 * @brief Constructs a Shader by compiling the given GLSL source and preparing the Vulkan resources.
 * @param source GLSL defining the public Shader function `void vulkan_main(Slice slice)`.
 * @param name Diagnostic name reported by shader compilation errors.
 */
Shader::Shader(std::string_view source, std::string_view name) {
    try {
        state_ = std::make_unique<ShaderState>(source, name);
    } catch (const vk::SystemError& error) {
        GPU_THROW(std::string("Vulkan Shader setup failed: ") + error.what());
    }
}
/** --------------------------------------------------------------------------------------------------------- Shader move */
Shader::Shader(Shader&& other) noexcept = default;
Shader& Shader::operator=(Shader&& other) noexcept = default;
/** --------------------------------------------------------------------------------------------------------- Shader::~Shader */
Shader::~Shader() = default;
/** --------------------------------------------------------------------------------------------------------- Shader::operator() */
std::shared_ptr<moodycamel::LightweightSemaphore> Shader::operator()(
    const GPUSlice* slices,
    size_t count,
    void (*callback)(GPUSlice slice),
    uint32_t workgroups
) const {
    if (!state_) GPU_THROW("Vulkan Shader: moved-from Shader cannot dispatch");
    // Public Shader remains synchronous; Kernel owns asynchronous scheduling and completion.
    state_->dispatch(slices, count, workgroups);
    if (callback != nullptr) {
        for (size_t i = 0; i < count; ++i) callback(slices[i].slice());
    }
    std::shared_ptr<moodycamel::LightweightSemaphore> done =
        std::make_shared<moodycamel::LightweightSemaphore>();
    done->signal();
    return done;
}
/** --------------------------------------------------------------------------------------------------------- Shader::operator() (one) */
std::shared_ptr<moodycamel::LightweightSemaphore> Shader::operator()(
    const GPUSlice& slice,
    void (*callback)(GPUSlice slice),
    uint32_t workgroups
) const {
    return (*this)(&slice, 1, callback, workgroups);
}
/** --------------------------------------------------------------------------------------------------------- GPU::run (Shader)
 * @brief Runs a prepared Shader over a list of slices, one workgroup column each.
 */
void GPU::run(const Shader& program, GPUSlice* streams, size_t stream_count) {
    program(streams, stream_count);
}
namespace {
/** --------------------------------------------------------------------------------------------------------- Encoded Program Header
 * @struct EncodedProgramHeader
 * @brief The 32-byte encoded-program header: magic, version, stream/pass shape, then the SPIR-V
 * words (padded to 8 bytes) and the diagnostic name (NUL-terminated, padded to 8) follow it.
 */
struct EncodedProgramHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t stream_count;
    uint32_t workgroups_hint;
    uint32_t pass_count;
    uint32_t register_seed_count;
    uint32_t constant_count;
    uint32_t reserved;
};
static_assert(sizeof(EncodedProgramHeader) == 32, "EncodedProgramHeader must be 32 bytes.");
constexpr uint32_t ENCODED_PROGRAM_MAGIC =
    uint32_t('N') | (uint32_t('B') << 8) | (uint32_t('G') << 16) | (uint32_t('P') << 24);
constexpr uint32_t ENCODED_PROGRAM_VERSION = 1u;
/** --------------------------------------------------------------------------------------------------------- shader_state_of
 * @brief Extracts the ShaderState* out of a Shader's single-pointer ABI (gpu.hpp's own static_assert).
 */
ShaderState* shader_state_of(const Shader& shader) {
    static_assert(sizeof(Shader) == sizeof(ShaderState*),
        "Shader must stay exactly one pointer for this extraction to be valid.");
    ShaderState* state;
    std::memcpy(&state, &shader, sizeof(ShaderState*));
    return state;
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- GPU::encode */
Slice GPU::encode(const Shader& program) {
    const ShaderState* state = shader_state_of(program);
    if (state == nullptr) GPU_THROW("GPU::encode: moved-from Shader cannot be encoded.");
    const std::vector<uint32_t>& spirv = state->spirv();
    const std::string& name = state->name();
    const size_t spirv_bytes = spirv.size() * sizeof(uint32_t);
    const size_t spirv_padded = (spirv_bytes + 7u) & ~size_t{7};
    const size_t name_bytes = name.size() + 1;
    const size_t name_padded = (name_bytes + 7u) & ~size_t{7};
    Slice encoded(sizeof(EncodedProgramHeader) + spirv_padded + name_padded);
    uint8_t* base = encoded.data<uint8_t>();
    /// stream_count 0: the list's own descriptor carries N at dispatch time.
    EncodedProgramHeader header{ENCODED_PROGRAM_MAGIC, ENCODED_PROGRAM_VERSION, 0u, 1u, 1u, 0u, 0u, 0u};
    std::memcpy(base, &header, sizeof(header));
    std::memcpy(base + sizeof(header), spirv.data(), spirv_bytes);
    std::memcpy(base + sizeof(header) + spirv_padded, name.c_str(), name_bytes);
    return encoded;
}
/** --------------------------------------------------------------------------------------------------------- GPU::decode */
Shader GPU::decode(const Slice&) {
    GPU_THROW("GPU::decode: blocked on a locked-header gap (report G0-5) — Shader has no "
        "accessible way to construct from a prepared ShaderState; needs friend struct GPU "
        "or a private Shader(std::unique_ptr<ShaderState>) constructor in gpu.hpp.");
}
namespace vulkan {
namespace {
/** --------------------------------------------------------------------------------------------------------- JobTableSlot
 * @struct JobTableSlot
 * @brief One cached job-table engine, keyed by its compiled program's stable host address (Law 3:
 * prepared once on first sight, looked up lock-free on every later GPU::run call).
 */
struct JobTableSlot {
    std::atomic<const void*> key{nullptr};
    std::atomic<ShaderState*> state{nullptr};
};
constexpr size_t JOB_TABLE_REGISTRY_SLOTS = 16;
constexpr size_t JOB_TABLE_ROUND_CAPACITY = 1024;
/** --------------------------------------------------------------------------------------------------------- job_table_engine_for
 * @brief Looks up, or on first sight prepares, the megakernel engine behind one compile_glsl program.
 * @param program A Slice returned by GPU::compile_glsl, expected to outlive every call using it.
 */
ShaderState& job_table_engine_for(const Slice& program) {
    static JobTableSlot registry[JOB_TABLE_REGISTRY_SLOTS];
    const void* key = program.raw();
    for (JobTableSlot& slot : registry) {
        const void* seen = slot.key.load(std::memory_order_acquire);
        if (seen == nullptr) {
            const void* expected = nullptr;
            if (slot.key.compare_exchange_strong(expected, key, std::memory_order_acq_rel)) {
                slot.state.store(
                    new ShaderState(program.data<uint32_t>(), program.size<uint32_t>(),
                        "vulkan_gpu_run_job_table", JOB_TABLE_ROUND_CAPACITY),
                    std::memory_order_release);
            } else if (expected != key) {
                continue;
            }
        } else if (seen != key) {
            continue;
        }
        ShaderState* state = slot.state.load(std::memory_order_acquire);
        while (state == nullptr) state = slot.state.load(std::memory_order_acquire);
        return *state;
    }
    GPU_THROW("GPU::run: job-table program registry is full (raise JOB_TABLE_REGISTRY_SLOTS)");
}
} // namespace
} // namespace vulkan
/** --------------------------------------------------------------------------------------------------------- GPU::run (job table)
 * @brief Runs a compile_glsl program with one stream per job, in rounds of the engine's table capacity.
 */
void GPU::run(const Slice& program, GPUSlice* streams, size_t stream_count) {
    auto& engine = vulkan::job_table_engine_for(program);
    const uint64_t host_handle[2] = {0, 0};
    for (size_t start = 0; start < stream_count; start += engine.capacity()) {
        const size_t round = std::min(engine.capacity(), stream_count - start);
        for (size_t i = 0; i < round; ++i) {
            engine.write_job(i, streams[start + i], host_handle);
        }
        engine.dispatch(round);
    }
}
/** --------------------------------------------------------------------------------------------------------- Prepare Vulkan Runtime
 * @brief Avoids waiting for a worker to dlopen the driver while extension import holds the loader lock.
 */
void VulkanStaticMethods::prepare_runtime() {
    static_cast<void>(VulkanContext::instance());
}
/** --------------------------------------------------------------------------------------------------------- Vulkan Allocator
 * @brief Allocates one mapped, device-addressable slab on the rung named by `context`.
 * @param size The slab size in bytes.
 * @param context The rung index (from the Placemat's `get_context`).
 * @return The handle whose substrate_handle is the VulkanBuffer.
 */
Placemat::Handle* VulkanStaticMethods::vulkan_allocator(size_t size, void* context) {
    const uint8_t rung = *static_cast<const uint8_t*>(context);
    VulkanBuffer* slab = new VulkanBuffer(size, VulkanContext::instance().placement_type_indices_[rung]);
    Placemat::Handle* handle = new Placemat::Handle();
    handle->substrate_handle = slab;
    handle->context = context;
    return handle;
}
/** --------------------------------------------------------------------------------------------------------- Deallocate
 * @brief Destroys the slab behind a handle; the alligator deletes the handle itself afterwards.
 * @param handle The handle to release.
 * @param context The rung index (unused; the slab knows its own memory).
 */
void VulkanStaticMethods::vulkan_deallocate(Placemat::Handle* handle, void* context) {
    static_cast<void>(context);
    delete static_cast<VulkanBuffer*>(handle->substrate_handle);
    handle->substrate_handle = nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Get Host Pointer
 * @brief The slab's persistent CPU mapping.
 * @param handle The handle to resolve.
 * @return The mapped host pointer.
 */
void* VulkanStaticMethods::vulkan_get_host_ptr(Placemat::Handle* handle) {
    return static_cast<VulkanBuffer*>(handle->substrate_handle)->host_;
}
/** --------------------------------------------------------------------------------------------------------- Get Vulkan Context
 * @brief The un-rung'd context hook: the default rung index.
 * @return A pointer to the HOST_VISIBLE rung index.
 */
void* VulkanStaticMethods::vulkan_get_context() {
    return VulkanPlacements::rung_context<static_cast<uint8_t>(PlacementIndex::HOST_VISIBLE)>();
}
/** --------------------------------------------------------------------------------------------------------- Placement table
 * @brief The Placemat behind each nebula placement: the heap built-in for the host-only rungs,
 * one Vulkan Placemat per device rung.
 */
const Placemat* const Placement::HOST = BuffetMenu::get("heap");
const Placemat* const Placement::HOST_VISIBLE = VulkanPlacements::rung<1>("vulkan_host_visible");
const Placemat* const Placement::HOST_CACHEABLE = VulkanPlacements::rung<2>("vulkan_host_cacheable");
const Placemat* const Placement::DEVICE = VulkanPlacements::rung<3>("vulkan_device");
const Placemat* const Placement::UNIFIED = VulkanPlacements::rung<4>("vulkan_unified");
const Placemat* const Placement::BASIC_HEAP = BuffetMenu::get("heap");
namespace {
/** --------------------------------------------------------------------------------------------------------- Publish GPU Slice
 * @brief Publishes a slice under a fresh pool reference so its CPU and GPU halves are index-linked.
 * @param slice The slice to publish.
 * @return The pool reference.
 */
uint32_t publish_gpu_slice(Slice slice) {
    CPUBufRef* reference = GlobalPool::instance().new_buf();
    reference->set(std::move(slice));
    return reference->my_idx();
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- GPUSlice Constructor
 * @brief Constructs a new `GPUSlice` with the specified size.
 * @param size The size of the slice in bytes.
 * @param novel_buffer If true, the slice is allocated as a novel buffer. Default is false.
 */
GPUSlice::GPUSlice(size_t size, bool novel_buffer) {
    meta_ = publish_gpu_slice(Slice(size, novel_buffer, VulkanContext::buffer_placement()));
}
/** ------------------------------------------------------------------------------------------- Constructor - Copy from External Memory
 * @brief Copies data from an external memory location into a new slice of memory in the
 * buffet alligator.
 * @param copy_from Pointer to the external memory to copy from.
 * @param size The size of the data to copy in bytes.
 * @param novel_buffer If true, then the slice is allocated as a novel buffer instead of being
 * a claim of a pre-allocated slab. This is ideal for slices that are long-lived to help
 * reduce fragmentation in the arena. Default is false.
 */
GPUSlice::GPUSlice(
    const void* copy_from,
    size_t size,
    bool novel_buffer
) {
    meta_ = publish_gpu_slice(Slice(size, novel_buffer, VulkanContext::buffer_placement()));
    std::memcpy(raw(), copy_from, size);
}
/** ------------------------------------------------------------------------------------------- Constructor - From Slice
 * @brief Constructs a `GPUSlice` from an existing `Slice` object.
 * @param slice The `Slice` object to construct from.
 */
GPUSlice::GPUSlice(Slice slice) {
    meta_ = publish_gpu_slice(std::move(slice));
}
/** ------------------------------------------------------------------------------------------- Assignment - From Slice
 * @brief Assigns a `Slice` object to the `GPUSlice`.
 * @param slice The `Slice` object to assign from.
 */
GPUSlice& GPUSlice::operator=(Slice slice) {
    free();
    meta_ = publish_gpu_slice(std::move(slice));
    return *this;
}
/** ------------------------------------------------------------------------------------------- Conversion - To Slice
 * @brief Converts the `GPUSlice` to a `Slice` object.
 * @return A `Slice` object representing the same memory as the `GPUSlice`.
 */
GPUSlice::operator Slice&() {
    if (meta_ == 0xFFFFFFFFu) [[unlikely]] {
        GPU_THROW("GPUSlice::operator Slice&() called on a null or freed GPUSlice");
    }
    return *GlobalPool::instance().get(meta_)->slice_ref();
}
/** ------------------------------------------------------------------------------------------- Conversion - To Const Slice
 * @brief Converts the `GPUSlice` to a const `Slice` object.
 * @return A const `Slice` object representing the same memory as the `GPUSlice`.
 */
GPUSlice::operator const Slice&() const {
    if (meta_ == 0xFFFFFFFFu) {
        static const Slice* null_slice = const_cast<const Slice*>(new Slice());
        return *null_slice;
    }
    return *GlobalPool::instance().get(meta_)->slice_ref();
}
/** ------------------------------------------------------------------------------------------- Constructor - Copy from GPUSlice
 * @brief Constructs a `GPUSlice` by copying from another `GPUSlice`.
 * @param other The `GPUSlice` to copy from.
 */
GPUSlice::GPUSlice(const GPUSlice& other)
: meta_(
    other.meta_ == 0xFFFFFFFFu ? 0xFFFFFFFFu : publish_gpu_slice(static_cast<const Slice&>(other))
) {}
/** ------------------------------------------------------------------------------------------- Assignment - From GPUSlice
 * @brief Assigns a `GPUSlice` object to the current `GPUSlice`.
 * @param other The `GPUSlice` object to assign from.
 */
GPUSlice& GPUSlice::operator=(const GPUSlice& other) {
    if (this == &other) {
        return *this;
    }
    free();
    meta_ = other.meta_ == 0xFFFFFFFFu ? 0xFFFFFFFFu : publish_gpu_slice(static_cast<const Slice&>(other));
    return *this;
}
/** ------------------------------------------------------------------------------------------- Constructor - Move from GPUSlice
 * @brief Constructs a `GPUSlice` by moving from another `GPUSlice`.
 * @param other The `GPUSlice` to move from.
 */
GPUSlice::GPUSlice(GPUSlice&& other) noexcept
: meta_(other.meta_) { other.meta_ = 0xFFFFFFFFu; }
/** ------------------------------------------------------------------------------------------- Assignment - Move from GPUSlice
 * @brief Assigns a `GPUSlice` object to the current `GPUSlice` by moving from another
 * `GPUSlice`.
 * @param other The `GPUSlice` object to move from.
 */
GPUSlice& GPUSlice::operator=(GPUSlice&& other) noexcept {
    free();
    meta_ = other.meta_;
    other.meta_ = 0xFFFFFFFFu;
    return *this;
}
/** ------------------------------------------------------------------------------------------- Placement
 * @brief Returns the memory placement type of the slice.
 * @return The `Placement` enum value representing the slice's memory placement.
 */
const Placemat* GPUSlice::placement() const {
    return meta_ == UINT32_MAX ? nullptr : GlobalPool::instance().get(meta_)->slice_ref()->placement();
}
/** ------------------------------------------------------------------------------------------- Raw accessors
 * @brief Use the buffet alligator's internal memory arena system to resolve the slice's
 * host-writable pointer to the underlying memory.
 * @return A pointer to the underlying memory of the slice.
 */
void* GPUSlice::raw() {
    return meta_ == 0xFFFFFFFFu ? nullptr : GlobalPool::instance().get(meta_)->ptr();
}
/** ------------------------------------------------------------------------------------------- Raw accessors - const
 * @brief Use the buffet alligator's internal memory arena system to resolve the slice's
 * host-writable pointer to the underlying memory, but as a read-only pointer.
 * @return A read-only pointer to the underlying memory of the slice.
 */
const void* GPUSlice::raw() const {
    return meta_ == 0xFFFFFFFFu ? nullptr : GlobalPool::instance().get(meta_)->ptr();
}
/** ------------------------------------------------------------------------------------------- Create new view
 * @brief Creates a new view of the slice, which is a sub-slice of the original slice. The new
 * view shares the same underlying memory and reference counter as the original slice. Using
 * the default parameters will create a new view that is essentially identical to the original
 * slice - a shared view that increments the reference counter and will keep the underlying
 * memory alive until all views are destroyed.
 * @param offset The offset in bytes from the start of the original slice to the start of the
 * new view.
 * @param length The length in bytes of the new view.
 * @return A new `GPUSlice` object that is a view of the original slice.
 */
GPUSlice GPUSlice::slice(size_t offset, size_t length) const {
    if (meta_ == 0xFFFFFFFFu) [[unlikely]] {
        GPU_THROW("GPUSlice::slice: Attempted to slice a null or freed GPUSlice.");
    }
    GPUSlice view;
    view.meta_ = publish_gpu_slice(GlobalPool::instance().get(meta_)->slice_ref()->slice(offset, length));
    return view;
}
/** ------------------------------------------------------------------------------------------- Size in bytes
 * @brief Returns the size of the slice in bytes.
 * @return The size of the slice in bytes.
 */
size_t GPUSlice::size_bytes() const {
    return meta_ == 0xFFFFFFFFu ? 0 : GlobalPool::instance().get(meta_)->size_bytes();
}
/** ------------------------------------------------------------------------------------------- Resize
 * @brief Resizes the slice to a new size. If `preserve_data` is true, the existing data in
 * the slice will be preserved up to the minimum of the old and new sizes. If `preserve_data`
 * is false, the existing data will be discarded and the slice will be reallocated. This can
 * be called on a freed or null slice, in which case it will behave like a normal constructor
 * and allocate a new slice of the specified size.
 * @param new_size The new size of the slice in bytes.
 * @param preserve_data Whether to preserve existing data in the slice. Default is true.
 * @param novel_buffer Whether to allocate a novel buffer even if the slice is not null.
 * Default is false.
 * @param placement The memory placement strategy to use. Default is `default_placement()`.
 */
void GPUSlice::resize(
    size_t new_size,
    bool preserve_data,
    bool novel_buffer
) {
    GPUSlice new_slice(new_size, novel_buffer);
    if (preserve_data && meta_ != 0xFFFFFFFFu) {
        std::memcpy(new_slice.raw(), raw(), std::min(size_bytes(), new_size));
    }
    *this = std::move(new_slice);
}
/** ------------------------------------------------------------------------------------------- Free
 * @brief Frees the underlying memory of the slice. This is called automatically when the
 * slice is destroyed, but can be called manually to free the memory early. After calling this
 * method, the slice will be null.
 */
void GPUSlice::free() {
    if (meta_ != 0xFFFFFFFFu) {
        GlobalPool::instance().erase(meta_);
        meta_ = 0xFFFFFFFFu;
    }
}
/** ------------------------------------------------------------------------------------------- Root slice
 * @brief Returns a reference to the root slice. This is useful when dealing with nested
 * slices or `SliceType` conceptual objects.
 * @return A reference to the root slice.
 */
Slice& GPUSlice::root_slice() {
    return static_cast<Slice&>(*this);
}
/** ------------------------------------------------------------------------------------------- Root slice (const)
 * @brief Returns a const reference to the root slice. This is useful when dealing with nested
 * slices or `SliceType` conceptual objects.
 * @return A const reference to the root slice.
 */
const Slice& GPUSlice::root_slice() const {
    return static_cast<const Slice&>(*this);
}
} // namespace nebula
