/** --------------------------------------------------------------------------------------------------------- Placement Wrappers
 * @file heapbuffet.cpp
 * @brief Mirrors C core placement registrations in the public C++ registry.
 */
#include <alligator.hpp>
extern "C" {
#include "core/ba_core.h"
}

namespace buffetalligator {
static_assert(sizeof(Placemat::Handle) == sizeof(ba_handle_t));
static_assert(offsetof(Placemat::Handle, substrate_handle) == offsetof(ba_handle_t, substrate_handle));
static_assert(offsetof(Placemat::Handle, context) == offsetof(ba_handle_t, context));
namespace {
static PlacementDescription descriptions[BA_MAX_PLACEMENTS];
/** --------------------------------------------------------------------------------------------------------- Placement Bridge
 * @brief Calls each registered C++ callback through its exact public function type.
 */
template<size_t Index>
struct PlacementBridge {
    /** ------------------------------------------------------------------------------------------- Allocate
     * @brief Wraps a public allocator result for the opaque C core.
     */
    static ba_handle_t* allocate(size_t bytes, void* context) {
        return reinterpret_cast<ba_handle_t*>(descriptions[Index].allocate(bytes, context));
    }
    /** ------------------------------------------------------------------------------------------- Deallocate
     * @brief Releases a substrate through its original callback type.
     */
    static void deallocate(ba_handle_t* handle, void* context) {
        descriptions[Index].deallocate(reinterpret_cast<Placemat::Handle*>(handle), context);
    }
    /** ------------------------------------------------------------------------------------------- Host Pointer
     * @brief Resolves a stable host pointer through its original callback type.
     */
    static void* host_pointer(ba_handle_t* handle) {
        return descriptions[Index].get_host_ptr(reinterpret_cast<Placemat::Handle*>(handle));
    }
    /** ------------------------------------------------------------------------------------------- Zero
     * @brief Rezeros a substrate through its original callback type.
     */
    static void zero(ba_handle_t* handle, uint64_t offset, uint64_t bytes, void* context) {
        descriptions[Index].zero(reinterpret_cast<Placemat::Handle*>(handle), offset, bytes, context);
    }
};
/** --------------------------------------------------------------------------------------------------------- Bridge Functions
 * @brief Groups correctly typed C entry points for one placement.
 */
struct BridgeFunctions {
    ba_alloc_fn allocate; ///< Allocation entry point.
    ba_free_fn deallocate; ///< Deallocation entry point.
    ba_host_ptr_fn host_pointer; ///< Host-address entry point.
    ba_zero_fn zero; ///< Rezero entry point.
};
/** --------------------------------------------------------------------------------------------------------- Make Bridges
 * @brief Generates static callback entry points for every supported placement.
 */
template<size_t... Indices>
constexpr auto make_bridges(std::index_sequence<Indices...>) {
    return std::array<BridgeFunctions, sizeof...(Indices)>{{
        {PlacementBridge<Indices>::allocate, PlacementBridge<Indices>::deallocate,
         PlacementBridge<Indices>::host_pointer, PlacementBridge<Indices>::zero}...
    }};
}
static constexpr auto bridges = make_bridges(std::make_index_sequence<BA_MAX_PLACEMENTS>{});
/** --------------------------------------------------------------------------------------------------------- Destroy Handle
 * @brief Deletes the C++ handle after its placement releases the substrate.
 */
void destroy_handle(ba_handle_t* handle) { delete reinterpret_cast<Placemat::Handle*>(handle); }
/** --------------------------------------------------------------------------------------------------------- Throw Status
 * @brief Logs and throws a placement-specific core failure.
 */
void throw_status(ba_status_t status, const char* name) {
    const std::string message = std::string(ba_status_name(status)) + ": " + name;
    LOG_ERROR_STREAM << message;
    ALLIGATOR_THROW(message);
}
}
/** --------------------------------------------------------------------------------------------------------- Register Type
 * @brief Registers a C descriptor and publishes its C++ placement wrapper.
 */
uint16_t BuffetMenu::register_type_unchecked(
    const char* name,
    size_t default_slab_size,
    size_t bump_alignment,
    Placemat::Handle* (*allocate)(size_t, void*),
    void (*deallocate)(Placemat::Handle*, void*),
    void* (*get_host_ptr)(Placemat::Handle*),
    void* (*get_context)(),
    bool set_as_default
) {
    PlacementDescription description;
    description.name = name;
    description.slab_bytes = default_slab_size;
    description.base_alignment = bump_alignment;
    description.allocate = allocate;
    description.deallocate = deallocate;
    description.get_host_ptr = get_host_ptr;
    description.get_context = get_context;
    description.set_as_default = set_as_default;
    return register_type(description);
}
/** --------------------------------------------------------------------------------------------------------- Register Description
 * @brief Publishes a placement with its resource limits and optional novel caching.
 */
uint16_t BuffetMenu::register_type(const PlacementDescription& description) {
    if (!instance().builtins_ready_.load(std::memory_order_acquire)) ensure_builtins_slow();
    if (!description.name) {
        LOG_ERROR_STREAM << "PlacementDescription.name is required";
        ALLIGATOR_THROW("PlacementDescription.name is required");
    }
    const char* name = description.name;
    const size_t default_slab_size = description.slab_bytes;
    const size_t bump_alignment = description.base_alignment;
    const auto allocate = description.allocate;
    const auto deallocate = description.deallocate;
    const auto get_host_ptr = description.get_host_ptr;
    const auto get_context = description.get_context;
    const bool set_as_default = description.set_as_default;
    ba_placement_desc_t descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.name = name;
    descriptor.slab_bytes = default_slab_size;
    descriptor.budget_bytes = description.budget_bytes;
    descriptor.novel_cache_bytes = description.novel_cache_bytes;
    descriptor.query_available = description.query_available;
    const uint32_t next_type = ba_placement_count();
    if (next_type >= BA_MAX_PLACEMENTS) throw_status(BA_E_PLACEMENT, name);
    descriptions[next_type] = description;
    const auto& bridge = bridges[next_type];
    descriptor.zero = description.zero ? bridge.zero : nullptr;
    if (bump_alignment > UINT32_MAX) throw_status(BA_E_ALIGNMENT, name);
    descriptor.base_alignment = static_cast<uint32_t>(bump_alignment);
    descriptor.flags = set_as_default ? BA_PLACEMENT_DEFAULT : 0;
    descriptor.alloc = allocate ? bridge.allocate : nullptr;
    descriptor.free = deallocate ? bridge.deallocate : nullptr;
    descriptor.host_ptr = get_host_ptr ? bridge.host_pointer : nullptr;
    descriptor.context = get_context;
    descriptor.destroy_handle = destroy_handle;
    uint32_t type;
    const ba_status_t status = ba_placement_register(&descriptor, &type);
    if (status != BA_OK) throw_status(status, name);
    ba_placement_describe(type, &descriptor);
    auto placement = std::unique_ptr<Placemat>(new Placemat());
    placement->name_ = name;
    placement->alligator_ = allocate;
    placement->deallocate_ = deallocate;
    placement->get_host_ptr_ = get_host_ptr;
    placement->get_context_ = get_context;
    placement->type_ = static_cast<uint16_t>(type);
    placement->core_type_ = type;
    placement->bump_alignment_ = static_cast<uint16_t>(bump_alignment);
    placement->default_slab_size_ = static_cast<uint32_t>(descriptor.slab_bytes >> 12);
    auto& menu = instance();
    menu.placement_indices_.emplace(name, type);
    menu.placements_.emplace_back(std::move(placement));
    default_placement_slot() = menu.placements_.at(ba_placement_default()).get();
    menu.notify_change_listeners();
    return static_cast<uint16_t>(type);
}
/** --------------------------------------------------------------------------------------------------------- Ensure Builtins
 * @brief Creates wrappers for the two core built-ins exactly once.
 */
void BuffetMenu::ensure_builtins_slow() {
    auto& menu = instance();
    bool expected = false;
    if (!menu.builtins_claimed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        while (!menu.builtins_ready_.load(std::memory_order_acquire)) std::this_thread::yield();
        return;
    }
    ba_init();
    for (uint32_t type = 0; type < 2; ++type) {
        ba_placement_desc_t descriptor;
        ba_placement_describe(type, &descriptor);
        auto placement = std::unique_ptr<Placemat>(new Placemat());
        placement->name_ = descriptor.name;
        placement->alligator_ = reinterpret_cast<Placemat::Handle* (*)(size_t, void*)>(descriptor.alloc);
        placement->deallocate_ = reinterpret_cast<void (*)(Placemat::Handle*, void*)>(descriptor.free);
        placement->get_host_ptr_ = reinterpret_cast<void* (*)(Placemat::Handle*)>(descriptor.host_ptr);
        placement->get_context_ = descriptor.context;
        placement->type_ = static_cast<uint16_t>(type);
        placement->core_type_ = type;
        placement->bump_alignment_ = static_cast<uint16_t>(descriptor.base_alignment);
        placement->default_slab_size_ = static_cast<uint32_t>(descriptor.slab_bytes >> 12);
        menu.placement_indices_.emplace(descriptor.name, type);
        menu.placements_.emplace_back(std::move(placement));
    }
    default_placement_slot() = menu.placements_.at(ba_placement_default()).get();
    menu.builtins_ready_.store(true, std::memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Default Placement
 * @brief Returns the wrapper corresponding to the C core default identifier.
 */
const Placemat*& BuffetMenu::default_placement() {
    if (!instance().builtins_ready_.load(std::memory_order_acquire)) ensure_builtins_slow();
    return default_placement_slot();
}
/** --------------------------------------------------------------------------------------------------------- Shutdown
 * @brief Explicitly stops the C allocator worker after application quiescence.
 */
void BuffetMenu::shutdown() { ba_shutdown(); }
}
