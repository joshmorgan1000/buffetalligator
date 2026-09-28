/** --------------------------------------------------------------------------------------------------------- Vulkan Tests
 * @file vulkan_test.cpp
 * @brief Verifies Vulkan buffers, mapped Slice claims, and nested view addressing and lifetime.
 */
#include <alligator.hpp>
#include "../functional_support.hpp"
#include <cstring>
#include <utility>

using namespace buffetalligator;
using functional::require;
/** --------------------------------------------------------------------------------------------------------- Vulkan Buffer
 * @brief Checks persistent mapped storage, zero initialization, and isolation of live Vulkan buffers.
 */
void vulkan_buffer() {
    constexpr size_t bytes = 4096;
    constexpr size_t last_word = bytes / sizeof(uint32_t) - 1;
    VulkanBuffer first(bytes);
    VulkanBuffer second(bytes);
    require(first.size() == bytes && second.size() == bytes, "Vulkan buffer size differs");
    require(first.host() != nullptr && second.host() != nullptr, "Vulkan buffer mapping is null");
    require(first.address() != 0 && second.address() != 0, "Vulkan buffer device address is zero");
    require(first.host() != second.host(), "live Vulkan buffers share a host mapping");
    require(first.address() != second.address(), "live Vulkan buffers share a device address");
    auto* first_words = static_cast<uint32_t*>(first.host());
    auto* second_words = static_cast<uint32_t*>(second.host());
    require(first_words[0] == 0 && first_words[512] == 0 && first_words[last_word] == 0,
        "first Vulkan buffer is not zero initialized");
    require(second_words[0] == 0 && second_words[512] == 0 && second_words[last_word] == 0,
        "second Vulkan buffer is not zero initialized");
    first_words[0] = 0x10203040u;
    first_words[512] = 0x50607080u;
    first_words[last_word] = 0x90abcdefu;
    second_words[512] = 0x12345678u;
    require(static_cast<const uint32_t*>(first.host())[0] == 0x10203040u
        && static_cast<const uint32_t*>(first.host())[512] == 0x50607080u
        && static_cast<const uint32_t*>(first.host())[last_word] == 0x90abcdefu,
        "Vulkan buffer mapped writes did not persist");
    require(second_words[0] == 0 && second_words[512] == 0x12345678u
        && second_words[last_word] == 0,
        "mapped writes crossed between independent Vulkan buffers");
    Slice addresses(2 * sizeof(uint64_t), false, Placemat::HOST_VISIBLE);
    addresses.data<uint64_t>()[0] = first.address();
    addresses.data<uint64_t>()[1] = second.address();
    constexpr std::string_view body = R"glsl(
void vulkan_main(Slice addresses) {
    if (gl_LocalInvocationIndex != 0u) return;
    uint64_t source_address = slice_load_u64(addresses, 0u);
    uint64_t destination_address = slice_load_u64(addresses, 1u);
    U32x4Array(destination_address + 1024ul).v[0] =
        U32x4Array(source_address).v[0] + uvec4(1u, 2u, 3u, 4u);
}
)glsl";
    Shader shader(body, "vulkan_raw_buffer_test");
    shader(addresses);
    require(second_words[256] == 0x10203041u && second_words[257] == 2
        && second_words[258] == 3 && second_words[259] == 4,
        "GPU buffer addresses did not resolve their persistent host mappings");
    require(second_words[255] == 0 && second_words[260] == 0
        && second_words[512] == 0x12345678u && first_words[0] == 0x10203040u,
        "GPU buffer writes changed the source or adjacent destination bytes");
}
/** --------------------------------------------------------------------------------------------------------- Vulkan Claims
 * @brief Checks shared-slab claims and accumulated Slice and SliceId offsets against the GPU table.
 */
void vulkan_claims() {
    constexpr size_t bytes = 1u << 20;
    Slice slice(bytes, false, Placemat::HOST_VISIBLE);
    require(slice.valid(), "claiming a HOST_VISIBLE slice failed");
    require(slice.size_bytes() == bytes, "claimed slice size differs");
    std::memset(slice.data(), 0x5A, bytes);
    require(static_cast<const uint8_t*>(slice.raw())[0] == 0x5A
        && static_cast<const uint8_t*>(slice.raw())[bytes - 1] == 0x5A,
        "mapped writes did not land");
    Slice view = slice.slice(64, 128);
    require(view.size_bytes() == 128 && view.valid(), "sub-slice of the mapped slice failed");
    const uint64_t address = VulkanKernel::device_address(slice);
    require(address != 0, "device address of a HOST_VISIBLE slice is zero");
    require(VulkanKernel::device_address(view) == address + 64,
        "sub-slice device address does not track its offset");
    Slice following(bytes, false, Placemat::HOST_VISIBLE);
    require(Alligator::plate_for(following) == Alligator::plate_for(slice),
        "consecutive claims did not share a Vulkan slab");
    const GPUBuf original_record = *Alligator::gpubuf_for(slice);
    const GPUBuf following_record = *Alligator::gpubuf_for(following);
    require(following_record.address == original_record.address,
        "consecutive claims changed the slab base address");
    require(following_record.offset == original_record.offset + bytes,
        "consecutive claims did not advance the GPU offset");
    require(VulkanKernel::device_address(following) == address + bytes,
        "a nonzero slab claim resolved to the wrong device address");
    require(following.data() == slice.data() + bytes,
        "a nonzero slab claim resolved to the wrong host pointer");
    Slice following_view = following.slice(64, 128);
    Slice nested_view = following_view.slice(16, 32);
    const GPUBuf nested_record = *Alligator::gpubuf_for(nested_view);
    require(nested_record.address == original_record.address,
        "nested slicing shifted the GPU slab base address");
    require(nested_record.offset == following_record.offset + 80 && nested_record.size == 32,
        "nested slicing did not accumulate the GPU offset and size");
    require(VulkanKernel::device_address(nested_view) == address + bytes + 80,
        "nested slicing resolved to the wrong device address");
    require(nested_view.data() == following.data() + 80,
        "nested slicing resolved to the wrong host pointer");
    Slice identified_view = SliceId(nested_view.pool_index()).slice(4, 8);
    const GPUBuf identified_record = *Alligator::gpubuf_for(identified_view);
    require(identified_record.address == original_record.address,
        "SliceId slicing shifted the GPU slab base address");
    require(identified_record.offset == following_record.offset + 84
        && identified_record.size == 8,
        "SliceId slicing did not accumulate the GPU offset and size");
    require(VulkanKernel::device_address(identified_view) == address + bytes + 84,
        "SliceId slicing resolved to the wrong device address");
    require(identified_view.data() == following.data() + 84,
        "SliceId slicing resolved to the wrong host pointer");
    require(Alligator::gpu_table() + identified_view.pool_index()
        == Alligator::gpubuf_for(identified_view),
        "the Slice pool index did not resolve the shared GPU record");
    require(Alligator::gpu_table_address() != 0
        && Alligator::gpu_table_address() == VulkanKernel::gpu_pool_address(),
        "the shader pool address does not resolve the shared GPU table");
    identified_view.data<uint32_t>()[0] = 0x12345678u;
    require(following.data<uint32_t>()[21] == 0x12345678u,
        "nested views did not write the original mapped storage");
    view.free();
    slice.free();
    require(!slice.valid(), "freeing the slice did not null it");
    require(VulkanKernel::device_address(slice) == 0,
        "a null Slice has a nonzero device address");
}
/** --------------------------------------------------------------------------------------------------------- Vulkan Novel Views
 * @brief Checks dedicated Vulkan buffer views retain their addresses and data across ownership changes.
 */
void vulkan_novel_views() {
    Slice root(256, true, Placemat::HOST_VISIBLE);
    Slice independent(256, true, Placemat::HOST_VISIBLE);
    require(root.valid(), "dedicated Vulkan Slice allocation failed");
    require(root.placement() == Placemat::HOST_VISIBLE, "dedicated Vulkan placement differs");
    require(Alligator::plate_for(root) != Alligator::plate_for(independent),
        "dedicated Vulkan Slices share an allocation");
    const GPUBuf root_record = *Alligator::gpubuf_for(root);
    const uint64_t root_address = VulkanKernel::device_address(root);
    require(root_record.address != 0 && root_record.address == root_address
        && root_record.offset == 0 && root_record.size == 256,
        "dedicated Vulkan Slice does not describe its complete buffer");
    require(VulkanKernel::device_address(independent) != root_address,
        "dedicated Vulkan Slices share a device address");
    require(root.data<uint32_t>()[0] == 0 && root.data<uint32_t>()[63] == 0,
        "dedicated Vulkan Slice is not zero initialized");
    root.data<uint32_t>()[10] = 0x11223344u;
    root.data<uint32_t>()[11] = 0x55667788u;
    root.data<uint32_t>()[19] = 0x99aabbccu;
    root.data<uint32_t>()[20] = 0xddeeff00u;
    Slice view = root.slice(32, 64);
    Slice nested = view.slice(12, 36);
    Slice remainder = SliceId(nested.pool_index()).slice(8);
    Slice tail = root.slice(252);
    const GPUBuf nested_record = *Alligator::gpubuf_for(nested);
    const GPUBuf remainder_record = *Alligator::gpubuf_for(remainder);
    require(nested_record.address == root_address && nested_record.offset == 44
        && nested_record.size == 36,
        "dedicated nested view changed its base or accumulated offset incorrectly");
    require(remainder_record.address == root_address && remainder_record.offset == 52
        && remainder_record.size == 28,
        "SliceId remainder changed its base, offset, or remaining size");
    require(nested.data() == root.data() + 44
        && VulkanKernel::device_address(nested) == root_address + 44,
        "dedicated nested host and device addresses disagree");
    require(remainder.data() == root.data() + 52
        && VulkanKernel::device_address(remainder) == root_address + 52,
        "SliceId remainder host and device addresses disagree");
    require(tail.size_bytes() == 4 && tail.data() == root.data() + 252
        && VulkanKernel::device_address(tail) == root_address + 252,
        "implicit-length Vulkan view did not reach the buffer end");
    require(nested.data<uint32_t>()[0] == 0x55667788u
        && nested.data<uint32_t>()[8] == 0x99aabbccu,
        "nested Vulkan view cannot read parent writes");
    remainder.data<uint32_t>()[0] = 0x31415926u;
    tail.data<uint32_t>()[0] = 0x27182818u;
    require(root.data<uint32_t>()[13] == 0x31415926u
        && root.data<uint32_t>()[63] == 0x27182818u,
        "Vulkan view writes did not reach their parent storage");
    require(root.data<uint32_t>()[10] == 0x11223344u
        && root.data<uint32_t>()[20] == 0xddeeff00u,
        "nested Vulkan writes changed adjacent storage");
    Slice copied(nested);
    Slice assigned;
    assigned = nested;
    require(copied.pool_index() != nested.pool_index()
        && assigned.pool_index() != nested.pool_index()
        && assigned.pool_index() != copied.pool_index(),
        "Vulkan Slice copies reused a live pool identity");
    require(copied.data() == nested.data() && assigned.data() == nested.data()
        && VulkanKernel::device_address(copied) == root_address + 44
        && VulkanKernel::device_address(assigned) == root_address + 44,
        "Vulkan Slice copies changed their mapped storage");
    const uint32_t copied_id = copied.pool_index();
    Slice moved(std::move(copied));
    require(!copied.valid() && moved.pool_index() == copied_id,
        "moving a Vulkan Slice did not transfer its pool identity");
    Slice retained;
    retained = std::move(moved);
    require(!moved.valid() && retained.pool_index() == copied_id,
        "move-assigning a Vulkan Slice did not transfer its pool identity");
    root.free();
    view.free();
    nested.free();
    remainder.free();
    tail.free();
    assigned.free();
    require(retained.valid() && retained.size_bytes() == 36,
        "freeing parents invalidated a retained dedicated Vulkan view");
    require(VulkanKernel::device_address(retained) == root_address + 44
        && Alligator::gpubuf_for(retained)->address == root_address
        && Alligator::gpubuf_for(retained)->offset == 44,
        "freeing parents changed the retained Vulkan view address");
    require(retained.data<uint32_t>()[0] == 0x55667788u
        && retained.data<uint32_t>()[2] == 0x31415926u
        && retained.data<uint32_t>()[8] == 0x99aabbccu,
        "freeing parents discarded the retained Vulkan view data");
    retained.data<uint32_t>()[8] = 0xabcdef01u;
    require(retained.data<uint32_t>()[8] == 0xabcdef01u,
        "the retained Vulkan view is no longer writable");
    retained.free();
    require(!retained.valid() && VulkanKernel::device_address(retained) == 0,
        "the released Vulkan view still has a device address");
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs one Vulkan allocation case or reports that the device is unavailable.
 */
int main(int count, char** arguments) {
    if (!VulkanKernel::available()) {
        Slice unavailable(128, true, Placemat::HOST_VISIBLE);
        Slice unavailable_view = unavailable.slice(16, 32);
        require(VulkanKernel::device_address(unavailable) == 0
            && VulkanKernel::device_address(unavailable_view) == 0,
            "an unavailable device returned a nonzero Slice address");
        LOG_INFO_STREAM << "Skipping Vulkan allocation tests: no Vulkan device is available";
        return 77;
    }
    LOG_INFO_STREAM << "Vulkan device: " << VulkanKernel::device_name();
    return functional::run(count, arguments, {
        {"vulkan_buffer", &vulkan_buffer},
        {"vulkan_claims", &vulkan_claims},
        {"vulkan_novel_views", &vulkan_novel_views}
    });
}
