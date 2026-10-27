/** --------------------------------------------------------------------------------------------------------- Vulkan Allocator Tests
 * @file vulkan_test.cpp
 * @brief Checks mapped Vulkan buffers and unit-aligned Slice views through production APIs.
 */
#include <alligator.hpp>
#include <alligator/easygpu.hpp>
#include <gpu/easyvulkan.hpp>
#include <core/ba_descriptor.h>
#include "../functional_support.hpp"
#include <cstdint>

using namespace buffetalligator;
using functional::require;
/** --------------------------------------------------------------------------------------------------------- Device Address
 * @brief Reads the device address a Slice carries to the GPU.
 */
static uint64_t device_address(const Slice& slice) {
    return reinterpret_cast<const ba_slice_t*>(&slice)->address;
}
/** --------------------------------------------------------------------------------------------------------- Buffer
 * @brief Verifies raw Vulkan allocation identity and host-visible shader writes.
 */
static void vulkan_buffer() {
    static_cast<void>(VulkanContext::buffer_placement());
    VulkanBuffer source(4096);
    VulkanBuffer destination(4096);
    require(VulkanBuffer::size_of(&source) == 4096 && VulkanBuffer::host_ptr(&source) && source.address(),
        "Vulkan allocation is incomplete");
    require(VulkanBuffer::host_ptr(&source) != VulkanBuffer::host_ptr(&destination)
        && source.address() != destination.address(), "Independent buffers share storage");
    auto* output = static_cast<uint32_t*>(VulkanBuffer::host_ptr(&destination));
    require(output[0] == 0 && output[1023] == 0, "Vulkan buffer was not initialized");
    static_cast<uint32_t*>(VulkanBuffer::host_ptr(&source))[0] = 41;
    Slice addresses(64, VulkanContext::buffer_placement());
    addresses.data<uint64_t>()[0] = source.address();
    addresses.data<uint64_t>()[1] = destination.address();
    Shader shader(R"glsl(
void alligator_main(Slice addresses) {
    if (gl_LocalInvocationIndex != 0u) return;
    U32Array(slice_load_u64(addresses, 1u)).v[0] = U32Array(slice_load_u64(addresses, 0u)).v[0] + 1u;
}
)glsl", "vulkan_buffer");
    shader(addresses).get();
    require(output[0] == 42 && output[1] == 0, "GPU writes did not preserve mapped buffer boundaries");
}
/** --------------------------------------------------------------------------------------------------------- Claims
 * @brief Checks successive slab claims advance by their bytes and views widen to their 64-byte units.
 */
static void vulkan_claims() {
    Slice first(4096, VulkanContext::buffer_placement());
    Slice following(4096, VulkanContext::buffer_placement());
    require(device_address(following) == device_address(first) + 4096,
        "Successive claims did not advance by the claim's bytes");
    Slice parent = following.slice(65, 126);
    Slice nested = parent.slice(3, 16);
    require(parent.size_bytes() == 128 && parent.capacity() == 128 && parent.data() == following.data() + 64,
        "A requested byte window did not widen to its 64-byte units: size ", parent.size_bytes(), " capacity ",
        parent.capacity(), " offset ", parent.data() - following.data());
    require(nested.size_bytes() == 64 && nested.capacity() == 64 && nested.data() == following.data() + 64,
        "A nested view did not keep its parent's unit: size ", nested.size_bytes(), " capacity ",
        nested.capacity(), " offset ", nested.data() - following.data());
    require(device_address(nested) == device_address(following) + 64,
        "A nested view's device address did not follow its host offset");
    nested.get_as<uint32_t>() = 77;
    require(following.data<uint32_t>()[16] == 77, "A view wrote the wrong host offset");
    Slice heap(64, BuffetDescriptors::descriptor<AlignedHeapBuffer>());
    require(device_address(heap) == reinterpret_cast<uint64_t>(heap.raw()),
        "Heap memory was exposed under a device address other than its host pointer");
}
/** --------------------------------------------------------------------------------------------------------- Novel Views
 * @brief Verifies dedicated allocation identity and retained views after parent destruction.
 */
static void vulkan_novel_views() {
    Slice root(256, true, VulkanContext::buffer_placement());
    Slice separate(256, true, VulkanContext::buffer_placement());
    const uint64_t address = device_address(root);
    require(address != device_address(separate), "Dedicated claims share their device address");
    require(root.is_novel() && root.size_bytes() == 256 && root.capacity() == 256,
        "Dedicated claim metadata differs from its request");
    Slice view = root.slice(70, 80);
    Slice nested = view.slice(67, 4);
    Slice tail = root.slice(252);
    require(view.size_bytes() == 128 && view.capacity() == 128 && nested.size_bytes() == 64
        && nested.capacity() == 64 && tail.size_bytes() == 64 && tail.capacity() == 64,
        "Dedicated views did not widen to whole units: view ", view.size_bytes(), "/",
        view.capacity(), " nested ", nested.size_bytes(), "/", nested.capacity(), " tail ", tail.size_bytes(),
        "/", tail.capacity());
    require(nested.data() == root.data() + 128 && tail.data() == root.data() + 192,
        "Dedicated views have incorrect host offsets");
    nested.get_as<uint64_t>() = 0x123456789ABCDEF0ull;
    Slice retained = nested;
    require(retained.raw() == nested.raw(), "A public Slice copy did not share its bytes");
    root.free();
    view.free();
    nested.free();
    tail.free();
    require(retained.size_bytes() == 64 && device_address(retained) == address + 128
        && retained.get_as<uint64_t>() == 0x123456789ABCDEF0ull, "A retained view lost its dedicated allocation");
    retained.free();
    require(device_address(retained) == 0, "A freed view retained a device address");
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs hardware-backed allocator checks only when Vulkan is available.
 */
int main(int count, char** arguments) {
    if (!VulkanKernel::available()) {
        LOG_INFO_STREAM << "Skipping Vulkan allocation tests: no Vulkan device is available";
        return 77;
    }
    return functional::run(count, arguments, {{"vulkan_buffer", &vulkan_buffer},
        {"vulkan_claims", &vulkan_claims}, {"vulkan_novel_views", &vulkan_novel_views}});
}
