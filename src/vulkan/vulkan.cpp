/** --------------------------------------------------------------------------------------------------------- Vulkan Implementation
 * @file vulkan.cpp
 * @brief The prepared Shader's bodies: preparation, the slot threads, recording, submission and the wait.
 */
#include <logging.hpp>
#include <alligator.hpp>
#include <alligator/easygpu.hpp>
#include <gpu/easyvulkan.hpp>
#include <gpu/backend.hpp>
#include <algorithm>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- EZ Vulkan Touch
 * @brief Freezes the context: probes the device and hands the buffer statics to VulkanBuffer, once.
 */
void ez_vulkan_touch() { static_cast<void>(VulkanContext::instance()); }
/// @brief Compiles GLSL compute source into SPIR-V words; defined in shader_source.cpp.
std::vector<uint32_t> shader_compile_glsl(std::string_view source, bool float16, std::string_view name);
/// @brief Makes the shader's writes available to the host, recorded after the dispatch.
static const vk::MemoryBarrier READBACK{vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eHostRead};
/** --------------------------------------------------------------------------------------------------------- Shader::prepare
 * @brief Compiles the body under the Slice ABI and builds the pipeline, one pool, command buffer and timeline per
 * slot, and the slot threads; a dispatch creates nothing.
 */
void Shader::prepare(std::string_view glsl, std::string_view name, uint32_t workgroups) {
    if (!VulkanContext::device_present()) ALLIGATOR_GPU_THROW("Shader preparation requires a Vulkan compute device");
    const DeviceProperties& properties = VulkanContext::device_properties();
    workgroups_.store(workgroups ? workgroups : std::max(1u, properties.max_workgroup_invocations / 64u),
        std::memory_order_relaxed);
    const std::vector<uint32_t> words =
        shader_compile_glsl(public_shader_source(glsl), properties.supports_float16, name);
    const vk::Device device = VulkanContext::device();
    const vk::PushConstantRange range(vk::ShaderStageFlagBits::eCompute, 0, sizeof(uint64_t));
    layout_ = device.createPipelineLayout(vk::PipelineLayoutCreateInfo({}, 0, nullptr, 1, &range));
    const vk::ShaderModule module =
        device.createShaderModule(vk::ShaderModuleCreateInfo({}, words.size() * sizeof(uint32_t), words.data()));
    const vk::PipelineShaderStageCreateInfo stage({}, vk::ShaderStageFlagBits::eCompute, module, "main");
    {
        std::lock_guard<std::mutex> lock(VulkanContext::pipeline_mutex());
        pipeline_ = device.createComputePipeline(VulkanContext::pipeline_cache(),
            vk::ComputePipelineCreateInfo({}, stage, layout_)).value;
    }
    device.destroyShaderModule(module);
    queue_ = VulkanContext::assign_queue();
    const vk::SemaphoreTypeCreateInfo timeline_type(vk::SemaphoreType::eTimeline, 0);
    vk::SemaphoreCreateInfo semaphore_info{};
    semaphore_info.pNext = &timeline_type;
    for (uint32_t slot = 0; slot < SLOTS; ++slot) {
        pools_[slot] = device.createCommandPool(vk::CommandPoolCreateInfo(
            vk::CommandPoolCreateFlagBits::eResetCommandBuffer, VulkanContext::queue_family(queue_)));
        commands_[slot] = device.allocateCommandBuffers(
            vk::CommandBufferAllocateInfo(pools_[slot], vk::CommandBufferLevel::ePrimary, 1))[0];
        timelines_[slot] = device.createSemaphore(semaphore_info);
    }
    for (uint32_t slot = 0; slot < SLOTS; ++slot) threads_[slot] = std::thread(&Shader::run, this, slot);
}
Shader::Shader(const ShaderSource& source, std::string_view name, uint32_t workgroups) {
    prepare(source.glsl, name, workgroups);
}
Shader::Shader(std::string_view glsl, std::string_view name, uint32_t workgroups) {
    prepare(glsl, name, workgroups);
}
/** --------------------------------------------------------------------------------------------------------- Shader::~Shader
 * @brief Stops each slot's thread after its last dispatch retires, then releases the slots and the pipeline.
 */
Shader::~Shader() {
    for (uint32_t slot = 0; slot < SLOTS; ++slot) dispatches_.enqueue(Dispatch{});
    for (std::thread& thread : threads_) thread.join();
    const vk::Device device = VulkanContext::device();
    for (uint32_t slot = 0; slot < SLOTS; ++slot) {
        device.destroySemaphore(timelines_[slot]);
        device.destroyCommandPool(pools_[slot]);
    }
    device.destroyPipeline(pipeline_);
    device.destroyPipelineLayout(layout_);
}
/** --------------------------------------------------------------------------------------------------------- Shader::run
 * @brief A slot's thread: the next dispatch recorded, submitted, waited and completed; a null result stops it.
 */
void Shader::run(uint32_t slot) {
    while (true) {
        Dispatch dispatch;
        dispatches_.wait_dequeue(dispatch);
        if (!dispatch.result) return;
        record(slot, dispatch.address, dispatch.groups_x, dispatch.groups_y);
        submit(slot);
        wait(slot);
        ShaderResult::complete(*dispatch.result);
        if (dispatch.held) static_cast<Slice*>(dispatch.single.raw())->~Slice();
    }
}
/** --------------------------------------------------------------------------------------------------------- Shader::record
 * @brief Records the slot for one dispatch: bind, the pushed address, the dispatch and the readback barrier.
 */
void Shader::record(uint32_t slot, uint64_t address, uint32_t groups_x, uint32_t groups_y) {
    vk::CommandBuffer command = commands_[slot];
    command.begin(vk::CommandBufferBeginInfo());
    command.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline_);
    command.pushConstants(layout_, vk::ShaderStageFlagBits::eCompute, 0, sizeof(address), &address);
    command.dispatch(groups_x, groups_y, 1);
    command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eHost,
        {}, 1, &READBACK, 0, nullptr, 0, nullptr);
    command.end();
}
/** --------------------------------------------------------------------------------------------------------- Shader::submit
 * @brief Submits the slot as recorded, signaling its next ticket when it retires.
 */
void Shader::submit(uint32_t slot) {
    ++tickets_[slot];
    const vk::TimelineSemaphoreSubmitInfo signal(0, nullptr, 1, &tickets_[slot]);
    vk::SubmitInfo info({}, {}, commands_[slot], timelines_[slot]);
    info.pNext = &signal;
    VulkanContext::submit(queue_, info);
}
/** --------------------------------------------------------------------------------------------------------- Shader::wait
 * @brief Blocks until the slot's last submit has retired.
 */
void Shader::wait(uint32_t slot) const {
    const vk::SemaphoreWaitInfo info({}, 1, &timelines_[slot], &tickets_[slot]);
    static_cast<void>(VulkanContext::device().waitSemaphores(info, UINT64_MAX));
}
/** --------------------------------------------------------------------------------------------------------- Shader::operator()
 * @brief One workgroup row per Slice in the list; row Y reads element Y's id through the pushed address.
 */
ShaderResult Shader::operator()(
    const Slice& slices,
    size_t count,
    void (*done)(std::shared_ptr<void>),
    std::shared_ptr<void> context
) const {
    ShaderResult result(done, std::move(context));
    dispatches_.enqueue(Dispatch{slices.address(), workgroups_.load(std::memory_order_relaxed),
        static_cast<uint32_t>(count), result.state_, Slice{}, false});
    return result;
}
/** --------------------------------------------------------------------------------------------------------- Shader::operator()
 * @brief One workgroup row whose id is the Slice's, through a one-id list held until retirement.
 */
ShaderResult Shader::operator()(
    const Slice& slice,
    void (*done)(std::shared_ptr<void>),
    std::shared_ptr<void> context
) const {
    ShaderResult result(done, std::move(context));
    Dispatch dispatch{0, workgroups_.load(std::memory_order_relaxed), 1u, result.state_, Slice(sizeof(Slice)), true};
    new (dispatch.single.raw()) Slice(slice);
    dispatch.address = dispatch.single.address();
    dispatches_.enqueue(std::move(dispatch));
    return result;
}
} // namespace buffetalligator
