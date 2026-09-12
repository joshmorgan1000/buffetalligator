/** --------------------------------------------------------------------------------------------------------- Vulkan Placements
 * @file vulkan.cpp
 * @brief Registers Vulkan property classes behind the existing Placemat and Slice interfaces.
 */
#include <alligator.hpp>
#include <mutex>
#include <cstdio>
#include <charconv>
extern "C" {
#include "core/ba_core.h"
#include "memory/ba_vulkan.h"
#include "network/ba_network.h"
}

namespace buffetalligator {
namespace {
std::mutex vulkan_registration;
std::array<const Placemat*, 16> vulkan_placements{};
std::array<std::atomic<bool>, BA_MAX_PLACEMENTS> vulkan_types{};
std::array<std::array<char, 24>, 16> vulkan_names{};
/** --------------------------------------------------------------------------------------------------------- Placement Callbacks
 * @brief Binds one Vulkan property class to the public placement registration callbacks.
 */
template <uint32_t Properties> struct VulkanPlacement {
    /** ------------------------------------------------------------------------------------------- Allocate
     * @brief Wraps a C Vulkan allocation in the framework's public allocation record.
     */
    static Placemat::Handle* allocate(size_t bytes, void*) {
        void* storage = ba_vk_allocate(bytes, Properties);
        if (!storage) return nullptr;
        auto* handle = new (std::nothrow) Placemat::Handle{storage, nullptr};
        if (!handle) ba_vk_free(storage);
        return handle;
    }
    /** ------------------------------------------------------------------------------------------- Deallocate
     * @brief Frees the allocation while the framework retains responsibility for its handle.
     */
    static void deallocate(Placemat::Handle* handle, void*) {
        ba_vk_free(handle->substrate_handle);
    }
    /** ------------------------------------------------------------------------------------------- Host Address
     * @brief Resolves a persistent mapping for the arena's Slice addressing.
     */
    static void* host(Placemat::Handle* handle) { return ba_vk_host(handle->substrate_handle); }
    /** ------------------------------------------------------------------------------------------- Zero
     * @brief Rezeros a retired range before the arena publishes it for reuse.
     */
    static void zero(Placemat::Handle* handle, uint64_t offset, uint64_t bytes, void*) {
        if (ba_vk_zero(handle->substrate_handle, offset, bytes))
            throw std::runtime_error("Vulkan could not zero recycled memory");
    }
    /** ------------------------------------------------------------------------------------------- Description
     * @brief Supplies callbacks for a single property class.
     */
    static PlacementDescription description() {
        PlacementDescription result;
        result.allocate = allocate;
        result.deallocate = deallocate;
        result.get_host_ptr = host;
        result.zero = zero;
        return result;
    }
};
/** --------------------------------------------------------------------------------------------------------- Descriptions
 * @brief Instantiates the fixed Vulkan property-class callbacks once.
 */
template <size_t... Indices> auto descriptions(std::index_sequence<Indices...>) {
    return std::array<PlacementDescription, sizeof...(Indices)>{
        VulkanPlacement<Indices>::description()...};
}
/** --------------------------------------------------------------------------------------------------------- Vulkan Record
 * @brief Rejects host-only placements before interpreting an allocation as Vulkan storage.
 */
void* vulkan_record(const Slice& slice) {
    if (!slice || !vulkan_types[slice.placement()->type()].load(std::memory_order_acquire))
        ALLIGATOR_THROW("This Slice does not have Vulkan storage");
    return Placemat::get_for(&slice)->substrate_handle;
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Placement
 * @brief Registers the requested Vulkan class during application startup.
 */
const Placemat* BuffetMenu::vulkan(VkMemoryPropertyFlags properties) {
    if (!ba_vk_supported(properties))
        ALLIGATOR_THROW("Requested Vulkan memory properties are unavailable");
    std::lock_guard lock(vulkan_registration);
    if (vulkan_placements[properties]) return vulkan_placements[properties];
    static const auto templates = descriptions(std::make_index_sequence<16>{});
    PlacementDescription description = templates[properties];
    std::snprintf(vulkan_names[properties].data(), vulkan_names[properties].size(), "Vulkan/%u",
                  properties);
    description.name = vulkan_names[properties].data();
    description.slab_bytes = 16u * 1024u * 1024u;
    description.base_alignment = 256;
    description.budget_bytes = ba_vk_budget(properties);
    const uint16_t type = register_type(description);
    vulkan_types[type].store(true, std::memory_order_release);
    vulkan_placements[properties] = get(type);
    return vulkan_placements[properties];
}
/** --------------------------------------------------------------------------------------------------------- Device
 * @brief Initializes and exposes the allocation device for caller-created GPU resources.
 */
VkDevice BuffetMenu::vulkan_device() {
    if (ba_vk_initialize()) ALLIGATOR_THROW("Vulkan device initialization failed");
    return ba_vk_device();
}
/** --------------------------------------------------------------------------------------------------------- Buffer
 * @brief Returns the original buffer and byte-exact Slice offset without transferring ownership.
 */
VkDescriptorBufferInfo Slice::vulkan_buffer() const {
    void* record = vulkan_record(*this);
    const auto offset = static_cast<const unsigned char*>(raw()) -
                        static_cast<const unsigned char*>(ba_vk_host(record));
    return {ba_vk_buffer(record), static_cast<VkDeviceSize>(offset), size_bytes()};
}
/** --------------------------------------------------------------------------------------------------------- Synchronize
 * @brief Transfers the selected range after the caller has completed any GPU use.
 */
void Slice::vulkan_sync(bool to_device) const {
    void* record = vulkan_record(*this);
    const auto offset = static_cast<const unsigned char*>(raw()) -
                        static_cast<const unsigned char*>(ba_vk_host(record));
    if (ba_vk_transfer(record, offset, size_bytes(), to_device))
        ALLIGATOR_THROW("Vulkan Slice transfer failed");
}
} // namespace buffetalligator
/** --------------------------------------------------------------------------------------------------------- Network Transfer
 * @brief Transfers Vulkan allocations and leaves ordinary host placements untouched.
 */
extern "C" int ba_vulkan_transfer(const ba_slice_t* slice, int to_device) {
    if (slice->meta == BA_NULL_META) return 0;
    if (!buffetalligator::vulkan_types[ba_slice_placement(slice)].load(std::memory_order_acquire))
        return 0;
    void* record = ba_slice_handle(slice)->substrate_handle;
    const size_t offset = static_cast<const unsigned char*>(slice->ptr) -
                          static_cast<const unsigned char*>(ba_vk_host(record));
    return ba_vk_transfer(record, offset, slice->meta >> BA_SLOT_BITS, to_device);
}
/** --------------------------------------------------------------------------------------------------------- Resolve Network Placement
 * @brief Recreates Vulkan property classes on demand and requires custom placements to be registered.
 */
extern "C" int ba_net_placement(const char* name) {
    try {
        if (const auto* placement = buffetalligator::BuffetMenu::get(std::string(name)))
            return placement->type();
        if (std::strncmp(name, "Vulkan/", 7)) return -1;
        uint32_t properties = 0;
        const char* end = name + std::strlen(name);
        auto result = std::from_chars(name + 7, end, properties);
        if (result.ec != std::errc{} || result.ptr != end) return -1;
        const auto* placement = buffetalligator::BuffetMenu::vulkan(properties);
        return std::strcmp(placement->name(), name) ? -1 : placement->type();
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Cannot receive Slice placement: " << error.what();
        return -1;
    }
}
