/** --------------------------------------------------------------------------------------------------------- Shader Slice Tests
 * @file shader_slice_test.cpp
 * @brief Exercises the prepared Shader: identity resolution, list rows, ring reuse, callbacks, and shared
 * submission from many threads.
 */
#include <alligator.hpp>
#include <alligator/easygpu.hpp>
#include <gpu/easyvulkan.hpp>
#include "functional_support.hpp"
#include <array>
#include <cstdint>
#include <latch>
#include <memory>
#include <thread>
#include <vector>

using namespace buffetalligator;
using functional::require;
/** --------------------------------------------------------------------------------------------------------- Identity Body
 * @brief Reports the row's id, the live row count, this row, the X workgroups, the address, and the size.
 */
static constexpr std::string_view IDENTITY_BODY = R"glsl(
void alligator_main(Slice stream) {
    if (gl_LocalInvocationIndex != 0u) return;
    slice_store_u32x4(stream, 0u, uvec4(uint(slice_address(stream)), vulkan_count(), vulkan_index(), gl_NumWorkGroups.x));
    slice_store_u64(stream, 2u, slice_address(stream));
    slice_store_u64(stream, 3u, slice_size(stream));
}
)glsl";
/** --------------------------------------------------------------------------------------------------------- Increment Body
 * @brief Adds one to the first word of the row's Slice.
 */
static constexpr std::string_view INCREMENT_BODY = R"glsl(
void alligator_main(Slice stream) {
    if (gl_LocalInvocationIndex == 0u) slice_store_u32(stream, 0u, slice_load_u32(stream, 0u) + 1u);
}
)glsl";
/** --------------------------------------------------------------------------------------------------------- Check Identity
 * @brief Verifies one row's words against the Slice the host dispatched.
 */
static void check_identity(const Slice& stream, uint32_t count, uint32_t index, uint32_t workgroups) {
    const uint32_t* words = stream.data<uint32_t>();
    const uint64_t* values = stream.data<uint64_t>();
    require(words[0] == uint32_t(stream.gpu().address + (uint64_t(stream.gpu().offset) << 6)),
        "The shader resolved a different Slice than the one dispatched");
    require(words[1] == count && words[2] == index, "The dispatch published the wrong row count or row");
    require(words[3] == workgroups, "The dispatch did not use the shader's configured workgroups");
    require(values[3] == stream.size_bytes(), "The shader did not widen granules to bytes");
}
/** --------------------------------------------------------------------------------------------------------- Shader Identity
 * @brief Dispatches one Slice and checks every published field, then again with a new workgroup count.
 */
static void shader_identity() {
    Shader shader(IDENTITY_BODY, "slice_identity", 3);
    require(shader.workgroups() == 3, "The constructor did not keep the configured workgroups");
    Slice payload(64, VulkanContext::buffer_placement());
    shader(payload).get();
    check_identity(payload, 1, 0, 3);
    shader.set_workgroups(2);
    shader(payload).get();
    check_identity(payload, 1, 0, 2);
}
/** --------------------------------------------------------------------------------------------------------- List Dispatch
 * @brief Binds four views of one slab as four rows and checks each row's identity and relative address.
 */
static void list_dispatch() {
    Shader shader(IDENTITY_BODY, "slice_list", 1);
    Slice storage(256, VulkanContext::buffer_placement());
    std::array<Slice, 4> streams{storage.slice(0, 64), storage.slice(64, 64), storage.slice(128, 64),
        storage.slice(192, 64)};
    Slice list = Alligator::claim(streams.size() * sizeof(Slice), false, VulkanContext::buffer_placement(),
        SliceTypes::register_type<Slice>());
    for (size_t index = 0; index < streams.size(); ++index) list.data<Slice>()[index] = streams[index];
    shader(list, streams.size()).get();
    const uint64_t base = streams[0].data<uint64_t>()[2];
    for (uint32_t index = 0; index < streams.size(); ++index) {
        check_identity(streams[index], 4, index, 1);
        require(streams[index].data<uint64_t>()[2] == base + 64 * index,
            "A view resolved to the wrong offset inside its slab");
    }
}
/** --------------------------------------------------------------------------------------------------------- Ring Wrap
 * @brief Submits far more dispatches than the shader has slots before waiting on any of them.
 */
static void ring_wrap() {
    Shader shader(INCREMENT_BODY, "slice_ring", 1);
    constexpr size_t rounds = 40;
    Slice payload(64, VulkanContext::buffer_placement());
    std::vector<ShaderResult> results;
    results.reserve(rounds);
    for (size_t round = 0; round < rounds; ++round) results.push_back(shader(payload));
    for (const ShaderResult& result : results) result.get();
    require(payload.get_as<uint32_t>() == rounds, "Reused slots lost or reordered dispatches");
}
/** --------------------------------------------------------------------------------------------------------- Shared Arrival
 * @brief The shared-context completion: counts down the latch inside the context.
 */
struct SharedArrival {
    std::latch done{1};
    static void arrive(std::shared_ptr<void> context) {
        static_cast<SharedArrival*>(context.get())->done.count_down();
    }
};
/** --------------------------------------------------------------------------------------------------------- Callbacks
 * @brief Checks both completion forms fire after the GPU wrote, and that get() returns the shared context.
 */
static void callbacks() {
    Shader shader(INCREMENT_BODY, "slice_callbacks", 1);
    Slice payload(64, VulkanContext::buffer_placement());
    auto shared = std::make_shared<SharedArrival>();
    ShaderResult result = shader(payload, &SharedArrival::arrive, shared);
    shared->done.wait();
    require(payload.get_as<uint32_t>() == 1, "The shared callback fired before the dispatch retired");
    require(result.get() == shared, "get() did not return the shared context");
    auto second = std::make_shared<SharedArrival>();
    std::array<Slice, 2> streams{payload.slice(), Slice(64, VulkanContext::buffer_placement())};
    Slice list = Alligator::claim(2 * sizeof(Slice), false, VulkanContext::buffer_placement(),
        SliceTypes::register_type<Slice>());
    list.data<Slice>()[0] = streams[0];
    list.data<Slice>()[1] = streams[1];
    shader(list, streams.size(), &SharedArrival::arrive, second);
    second->done.wait();
    require(payload.get_as<uint32_t>() == 2 && streams[1].get_as<uint32_t>() == 1,
        "The list callback fired before every row retired");
}
/** --------------------------------------------------------------------------------------------------------- Submitter
 * @brief One thread's share of a prepared Shader: its own Slice incremented in a loop.
 */
struct Submitter {
    Shader* shader;
    Slice stream;
    static void run(Submitter* submitter) {
        for (size_t round = 0; round < 16; ++round) (*submitter->shader)(submitter->stream).get();
    }
};
/** --------------------------------------------------------------------------------------------------------- Concurrent Dispatch
 * @brief More submitting threads than queues share one Shader; every Slice must see exactly its own work.
 */
static void concurrent_dispatch() {
    Shader shader(INCREMENT_BODY, "slice_concurrent", 1);
    const size_t count = std::max<size_t>(2, std::thread::hardware_concurrency()) + 2;
    std::vector<std::unique_ptr<Submitter>> submitters;
    std::vector<std::thread> threads;
    for (size_t index = 0; index < count; ++index) {
        auto submitter = std::make_unique<Submitter>();
        submitter->shader = &shader;
        submitter->stream = Slice(64, VulkanContext::buffer_placement());
        submitters.push_back(std::move(submitter));
    }
    for (auto& submitter : submitters) threads.emplace_back(&Submitter::run, submitter.get());
    for (std::thread& thread : threads) thread.join();
    for (const auto& submitter : submitters)
        require(submitter->stream.get_as<uint32_t>() == 16, "Concurrent submitters crossed their slots");
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs one hardware-backed Shader contract case.
 */
int main(int count, char** arguments) {
    if (!VulkanKernel::available()) {
        LOG_INFO_STREAM << "Skipping shader Slice tests: no Vulkan compute device";
        return 77;
    }
    return functional::run(count, arguments, {{"vulkan_shader_identity", &shader_identity},
        {"vulkan_list_dispatch", &list_dispatch}, {"vulkan_ring_wrap", &ring_wrap},
        {"vulkan_callbacks", &callbacks}, {"vulkan_concurrent_dispatch", &concurrent_dispatch}});
}
