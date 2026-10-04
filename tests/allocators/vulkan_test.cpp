/** --------------------------------------------------------------------------------------------------------- Vulkan Allocator Tests
 * @file vulkan_test.cpp
 * @brief Checks mapped Vulkan buffers and granule-rounded Slice views through production APIs.
 */
#include <alligator.hpp>
#include <alligator/easygpu.hpp>
#include <alligator/easyvulkan.hpp>
#include <alligator/kitchen.hpp>
#include "../functional_support.hpp"
#include <cstdint>
#include <utility>

using namespace buffetalligator;
using functional::require;
/** --------------------------------------------------------------------------------------------------------- Buffer
 * @brief Verifies raw Vulkan allocation identity and host-visible shader writes.
 */
static void vulkan_buffer() {
    VulkanBuffer source(4096);
    VulkanBuffer destination(4096);
    require(source.size() == 4096 && source.host() && source.address(), "Vulkan allocation is incomplete");
    require(source.host() != destination.host() && source.address() != destination.address(),
        "Independent buffers share storage");
    auto* output = static_cast<uint32_t*>(destination.host());
    require(output[0] == 0 && output[1023] == 0, "Vulkan buffer was not initialized");
    static_cast<uint32_t*>(source.host())[0] = 41;
    Slice addresses(64, VulkanContext::buffer_placement());
    addresses.data<uint64_t>()[0] = source.address();
    addresses.data<uint64_t>()[1] = destination.address();
    Shader shader(R"glsl(
void alligator_main(Slice addresses) {
    if (gl_LocalInvocationIndex != 0u) return;
    U32Array(slice_load_u64(addresses, 1u)).v[0] = U32Array(slice_load_u64(addresses, 0u)).v[0] + 1u;
}
)glsl", "vulkan_buffer");
    OrderCountdown complete;
    ShaderResult result;
    shader(addresses, result, &OrderCountdown::arrive, &complete);
    complete.wait();
    result.rethrow();
    require(output[0] == 42 && output[1] == 0, "GPU writes did not preserve mapped buffer boundaries");
}
/** --------------------------------------------------------------------------------------------------------- Claims
 * @brief Checks granule records and region-directory lookup for successive slab claims.
 */
static void vulkan_claims() {
    Slice first(4096, VulkanContext::buffer_placement());
    Slice following(4096, VulkanContext::buffer_placement());
    const GPUBuf before = *Alligator::gpubuf_for(first);
    const GPUBuf after = *Alligator::gpubuf_for(following);
    require(before.address == after.address && after.offset == before.offset + 64,
        "Successive claims did not retain granule-based slab offsets");
    require(VulkanKernel::device_address(following) == VulkanKernel::device_address(first) + 4096,
        "Device addressing did not widen granules into bytes");
    Slice parent = following.slice(65, 126);
    Slice nested = parent.slice(3, 16);
    require(parent.size_bytes() == 128 && parent.data() == following.data() + 64,
        "A requested byte window did not round outward");
    require(nested.size_bytes() == 64 && nested.data() == following.data() + 64,
        "A nested view did not preserve rounded parent bounds");
    require(Alligator::gpubuf_for(nested)->size == 1
        && Alligator::gpubuf_for(nested)->offset == after.offset + 1,
        "Nested views stored byte counts in granule fields");
    require(Alligator::gpu_table(uint8_t((nested.id() >> 3) & 63)) + (nested.id() >> 9)
        == Alligator::gpubuf_for(nested), "Region lookup addressed the wrong GPU record");
    require(Alligator::gpu_directory_address() == VulkanKernel::gpu_pool_address()
        && VulkanKernel::gpu_pool_address() != 0, "The GPU region directory is unavailable");
    nested.get_as<uint32_t>() = 77;
    require(following.data<uint32_t>()[16] == 77, "A rounded view wrote the wrong host offset");
    Slice heap(64, BuffetDescriptors::get(0));
    require(VulkanKernel::device_address(heap) == 0, "Heap memory was exposed as a device address");
}
/** --------------------------------------------------------------------------------------------------------- Novel Views
 * @brief Verifies dedicated allocation identity and retained rounded views after parent destruction.
 */
static void vulkan_novel_views() {
    Slice root(256, true, VulkanContext::buffer_placement());
    Slice separate(256, true, VulkanContext::buffer_placement());
    const uint64_t address = VulkanKernel::device_address(root);
    require(address != VulkanKernel::device_address(separate), "Dedicated claims share their device address");
    require(Alligator::gpubuf_for(root)->size == 4 && Alligator::gpubuf_for(root)->offset == 0,
        "Dedicated claim metadata is not expressed in granules");
    Slice view = root.slice(70, 80);
    Slice nested = view.slice(67, 4);
    Slice tail = root.slice(252);
    require(view.size_bytes() == 128 && nested.size_bytes() == 64 && tail.size_bytes() == 64,
        "Dedicated views did not round their boundaries outward");
    require(nested.data() == root.data() + 128 && tail.data() == root.data() + 192,
        "Rounded dedicated views have incorrect host offsets");
    nested.get_as<uint64_t>() = 0x123456789ABCDEF0ull;
    Slice retained = nested;
    require(retained.id() != nested.id(), "A public Slice copy reused an occupied table identity");
    root.free(); view.free(); nested.free(); tail.free();
    require(retained.size_bytes() == 64 && VulkanKernel::device_address(retained) == address + 128
        && retained.get_as<uint64_t>() == 0x123456789ABCDEF0ull,
        "A retained view lost its dedicated allocation");
    retained.free();
    require(VulkanKernel::device_address(retained) == 0, "A freed view retained a device address");
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
