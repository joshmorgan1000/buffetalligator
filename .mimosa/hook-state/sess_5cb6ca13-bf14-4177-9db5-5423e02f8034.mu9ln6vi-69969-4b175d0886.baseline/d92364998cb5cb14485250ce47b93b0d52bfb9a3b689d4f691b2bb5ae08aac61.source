#pragma once
/** --------------------------------------------------------------------------------------------------------- VulkanBuffer
 * @file vulkanbuffer.hpp
 * @brief Vulkan slab provider for the Nebula Buffer model: allocates mapped, device-addressable,
 * zero-initialized storage buffers and destroys them when a slab's reference count drains.
 */
#include <logging.hpp>
#include <alligator.hpp>
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.hpp>

namespace buffetalligator {
class VulkanContext; struct GPUBuf;
/** --------------------------------------------------------------------------------------------------------- VulkanBuffer
 * @class VulkanBuffer
 * @brief The Vulkan handles behind one slab: buffer, memory, mapping, and device address.
 */
class VulkanBuffer {
private:
    vk::Buffer buffer_{};          ///< Vulkan buffer handle (the compute target).
    vk::DeviceMemory memory_{};    ///< Backing memory allocation.
    void* host_ = nullptr;         ///< Persistent CPU mapping.
    GPUBuf* gpu_buf_ = nullptr;    ///< Pointer to the GPU buffer representation.
    VulkanBuffer() = default;
    friend class VulkanContext;
    friend struct VulkanStaticMethods;
public:
    /** ------------------------------------------------------------------------------------------- Allocating Constructor
     * @brief The five-call Vulkan allocation: create, get requirements, allocate, bind, map, plus
     * the device-address query and the zero-fill the slab contract requires.
     * @param size_bytes Buffer size in bytes.
     */
    VulkanBuffer(size_t size_bytes);
    /** ------------------------------------------------------------------------------------------- Allocating Constructor (typed)
     * @brief The same five-call allocation against an explicit memory type index.
     * @param size_bytes Buffer size in bytes.
     * @param memory_type_index The memory type to allocate from.
     */
    VulkanBuffer(size_t size_bytes, uint32_t memory_type_index);
    /** ------------------------------------------------------------------------------------------- No copy/move */
    VulkanBuffer(const VulkanBuffer&) = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;
    VulkanBuffer(VulkanBuffer&&) = delete;
    VulkanBuffer& operator=(VulkanBuffer&&) = delete;
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Unmap and destroy the Vulkan objects. Defined in vulkan.cpp.
     */
    ~VulkanBuffer();
    /** ------------------------------------------------------------------------------------------- address
     * @brief Retrieves the device address of the Vulkan buffer.
     * @return The device address.
     */
    uint64_t address() const;
    /** ------------------------------------------------------------------------------------------- size
     * @brief Retrieves the size of the Vulkan buffer.
     * @return The size of the buffer in bytes.
     */
    uint64_t size() const;
    /** ------------------------------------------------------------------------------------------- host
     * @brief Retrieves the host pointer of the Vulkan buffer.
     * @return The host pointer.
     */
    void* host() const;
};
/** --------------------------------------------------------------------------------------------------------- Vulkan Static Methods
 * @struct VulkanStaticMethods
 * @brief Provides static methods for Vulkan buffer management.
 */
struct VulkanStaticMethods {
    VulkanStaticMethods() = delete; // Prevent instantiation of this static-only class.
    /** ------------------------------------------------------------------------------------------- Prepare Runtime
     * @brief Loads the Vulkan driver on the registering thread before the allocator worker starts.
     */
    static void prepare_runtime();
    /** ------------------------------------------------------------------------------------------- Vulkan Allocator
     * @brief Allocates a mapped Vulkan buffer of the specified size.
     * @param size The size of the Vulkan buffer to allocate.
     * @param context The rung index pointer the placement handed us.
     * @return The host mapping and the VulkanBuffer handle pair.
     */
    static std::pair<void*, void*> vulkan_allocator(size_t size, void* context);
    /** ------------------------------------------------------------------------------------------- Deallocate
     * @brief Destroys a Vulkan buffer and clears its host mapping.
     * @param host_ptr The host mapping of the buffer.
     * @param substrate_handle The VulkanBuffer handle backing the mapping.
     * @return The cleared host pointer and substrate handle pair.
     */
    static std::pair<void*, void*> vulkan_deallocate(void* host_ptr, void* substrate_handle);
    /** ------------------------------------------------------------------------------------------- Get Vulkan Context
     * @brief Retrieves the Vulkan context.
     * @return A pointer to the Vulkan context.
     */
    static void* vulkan_get_context();
};
/** --------------------------------------------------------------------------------------------------------- PlacementIndex
 * @enum PlacementIndex
 * @brief The memory-type ladder rungs `VulkanContext` resolves, one per Placemat below.
 */
enum class PlacementIndex : uint8_t {
    HOST = 0, HOST_VISIBLE = 1, HOST_CACHEABLE = 2, DEVICE = 3, UNIFIED = 4, BASIC_HEAP = 5, UNSPECIFIED = 6, COUNT = 7
};
/** --------------------------------------------------------------------------------------------------------- VulkanPlacements
 * @struct VulkanPlacements
 * @brief One Vulkan Placemat per ladder rung; the rung rides in the Placemat's context word and
 * the allocator resolves it to the memory type `VulkanContext` probed for it.
 */
struct VulkanPlacements {
    VulkanPlacements() = delete;
    /** ------------------------------------------------------------------------------------------- rung_context
     * @brief The `get_context` hook for one rung: a pointer to that rung's index.
     */
    template <uint8_t Rung>
    static void* rung_context() {
        VulkanStaticMethods::prepare_runtime();
        static uint8_t rung = Rung;
        return &rung;
    }
    /** ------------------------------------------------------------------------------------------- rung
     * @brief Registers (once) and returns the Vulkan Placemat for a ladder rung.
     */
    template <uint8_t Rung>
    static const Placemat* rung(const char* name) {
        static const Placemat* placemat = BuffetMenu::get(BuffetMenu::register_type(
            name, 64 * 1024 * 1024, 4096,
            &VulkanStaticMethods::vulkan_allocator, &VulkanStaticMethods::vulkan_deallocate,
            &VulkanPlacements::rung_context<Rung>, false));
        return placemat;
    }
    /** ------------------------------------------------------------------------------------------- Prime
     * @brief Registers every ladder rung before the first Slice allocates; the alligator's tracker
     * sizes its per-placement tables at the first slab, so a rung first touched later would index
     * past them (contract: process-lifetime Placemats register before the first Slice).
     */
    static void prime() {
        rung<0>("vulkan_host");
        rung<1>("vulkan_host_visible");
        rung<2>("vulkan_host_cacheable");
        rung<3>("vulkan_device");
        rung<4>("vulkan_unified");
        rung<5>("vulkan_basic_heap");
        rung<6>("vulkan_buffer");
    }
};
} // namespace buffetalligator
