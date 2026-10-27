#pragma once
/** --------------------------------------------------------------------------------------------------------- Type Utilities for Memory Slices
 * @file slicetype.hpp
 * @brief Type utilities for memory slices.
 */
#include <logging.hpp>
#include <alligator.hpp>
#include <memory/chainbuffet.hpp>
#include <absl/container/flat_hash_map.h>
#include <cstdint>
#include <shared_mutex>

namespace buffetalligator {
namespace types {
namespace constants {
/// @brief Starting bit for the alignment flags within the type field.
inline static constexpr uint32_t ALIGNMENT_BITS      = 0;
/// @brief Array of possible alignment values corresponding to the alignment bits.
inline static constexpr size_t   ALIGNMENTS[4]       = {1, 64, 256, 4096};
/// @brief Bit for the status flag within the type field.
inline static constexpr uint32_t STATUS_BIT          = 2;
/// @brief Bit for the SliceMap indicator within the type field.
inline static constexpr uint32_t SLICEMAP_BIT        = 3;
/// @brief Bit for the atomic flag within the type field.
inline static constexpr uint32_t ATOMIC_BIT          = 4;
/// @brief Bit for the slice-of-slices indicator within the type field,
/// first 16 bytes contains the length (how many slices) and then 8 bytes
/// of padding to keep the structure aligned well to 64 bytes.
inline static constexpr uint32_t SLICE_OF_SLICES_BIT = 5;
/// @brief Starting bit for the location flags within the type field.
inline static constexpr uint32_t LOCATION_BITS       = 6;
/// @brief Starting bit for the ownership flags within the type field.
inline static constexpr uint32_t OWNERSHIP_BITS      = 8;
/// @brief Starting bit for the structure flags within the type field.
inline static constexpr uint32_t STRUCTURE_BITS      = 10;
/// @brief Starting bit for the primitive type flags within the type field.
inline static constexpr uint32_t PRIMITIVE_BITS      = 12;
/// @brief Starting bit for the state flags within the type field.
inline static constexpr uint32_t STATE_BITS          = 14;
/// @brief Starting bit for the bitwidth flags within the type field.
inline static constexpr uint32_t BITWIDTH_BITS       = 16;
/// @brief Array of possible bitwidth values corresponding to the number of
/// bits per element. Yes, we may have gotten a little creative with some
/// of the potential values here, but we had some enum space left to fill.
inline static constexpr size_t   BITWIDTHS[16]       = {
    1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64, 128
};
/// @brief Starting bit for the semantic flags within the type field.
inline static constexpr uint32_t SEMANTIC_BITS       = 20;
} // namespace constants
/** --------------------------------------------------------------------------------------------------------- Status
 * @enum Status
 * @brief Represents the status of a memory slice, indicating whether it is null or valid.
 */
enum class Status : uint32_t {
    /// @brief Memory slice is in an undefined or null state.
    Null                                            = 0,
    /// @brief Memory slice is in a valid state.
    Valid                                           = 1 << constants::STATUS_BIT
};
/** --------------------------------------------------------------------------------------------------------- State
 * @enum State
 * @brief Represents the encryption and compression state of a memory slice.
 */
enum class State : uint32_t {
    /// @brief Data is in its raw, unprocessed form.
    Raw                                             = 0,
    /// @brief Data is encrypted using AES-256-GCM.
    Encrypted                                       = 1 << constants::STATE_BITS,
    /// @brief Data is compressed using zlib compression.
    Compressed                                      = 2 << constants::STATE_BITS,
    /// @brief Compression is always applied before encryption.
    EncryptedAndCompressed                          = 3 << constants::STATE_BITS
};
/** --------------------------------------------------------------------------------------------------------- PrimitiveType
 * @enum PrimitiveType
 * @brief Represents the primitive type of the element or elements in the slice.
 */
enum class PrimitiveType : uint32_t {
    /// @brief Data is numeric, unsigned, and integral.
    Unsigned                                        = 0,
    /// @brief Contains character data (as opposed to numeric).
    Text                                            = 1 << constants::PRIMITIVE_BITS,
    /// @brief Data is numeric, signed, and integral.
    Signed                                          = 2 << constants::PRIMITIVE_BITS,
    /// @brief Data is floating-point (has a fractional component).
    FloatingPoint                                   = 3 << constants::PRIMITIVE_BITS,
};
/** --------------------------------------------------------------------------------------------------------- Structure
 * @enum Structure
 * @brief Represents the structural type of the element or elements in the slice.
 */
enum class Structure : uint32_t {
    /// @brief Represents a single element (not a collection, list, or array).
    SingleElement                                   = 0,
    /// @brief Represents a list or array of elements.
    List                                            = 1 << constants::STRUCTURE_BITS,
    /// @brief Represents a single part of a multi-slice sequence.
    Chunked                                         = 2 << constants::STRUCTURE_BITS,
    /// @brief This slice is a JSON representation of the data.
    JSON                                            = 3 << constants::STRUCTURE_BITS
};
/** --------------------------------------------------------------------------------------------------------- Alignment
 * @enum Alignment
 * @brief Represents the memory alignment of the element or elements in the slice.
 */
enum class Alignment : uint32_t {
    /// @brief Single-byte alignment (unaligned).
    Unaligned                                       = 0,
    /// @brief 64-byte alignment.
    VectorAligned                                   = 1 << constants::ALIGNMENT_BITS,
    /// @brief 256-byte alignment.
    MatrixAligned                                   = 2 << constants::ALIGNMENT_BITS,
    /// @brief 4096-byte alignment.
    PageAligned                                     = 3 << constants::ALIGNMENT_BITS
};
/** --------------------------------------------------------------------------------------------------------- Ownership
 * @enum Ownership
 * @brief Represents the ownership model of the slice.
 */
enum class Ownership : uint32_t {
    /// @brief Weak reference, not owned by this slice.
    Weak                                            = 0,
    /// @brief This slice is solely responsible for releasing the memory that
    /// it owns.
    Owned                                           = 1 << constants::OWNERSHIP_BITS,
    /// @brief Slice that shares the memory with other slices and uses reference
    /// counting.
    Shared                                          = 2 << constants::OWNERSHIP_BITS,
    /// @brief Parent object is a ChainBuffet; part of the pre-allocated slab
    /// from the Alligator's memory arena.
    Pooled                                          = 3 << constants::OWNERSHIP_BITS
};
/** --------------------------------------------------------------------------------------------------------- Location
 * @enum Location
 * @brief Represents the location of the memory.
 */
enum class Location : uint32_t {
    /// @brief Heap allocated memory.
    Heap                                            = 0,
    /// @brief Memory allocated in the Alligator's arena.
    FileMapped                                      = 1 << constants::LOCATION_BITS,
    /// @brief Memory mapped from a file.
    HostCoherent                                    = 2 << constants::LOCATION_BITS,
    /// @brief Memory is immediately accessible by the GPU (e.g. a native
    /// VulkanBuffer).
    DeviceLocal                                     = 3 << constants::LOCATION_BITS
};
/** --------------------------------------------------------------------------------------------------------- Access Synchronization Status
 * @enum Sync
 * @brief Represents the synchronization requirements of the memory slice.
 */
enum class Sync : uint32_t {
    /// @brief No special synchronization required for access.
    None                                            = 0,
    /// @brief Memory should be accessed using `std::atomic_ref`.
    Atomic                                          = 1 << constants::ATOMIC_BIT
};
/// @brief Function signature for constructing a `Slice` of the given type word from the arguments.
using SliceConstructFn = Slice (*)(uint32_t type, ArgumentsHolder&& args);
/// @brief Function signature for cutting a `Slice` of the given type word from a parent: the `Slice` being cut
/// for a handle or a view, the backing for a root.
using SubSliceFn = Slice (*)(uint32_t type, void* parent, size_t offset, size_t length);
/// @brief Function signature for copy constructor/assignment.
using SliceCopyFn = void (*)(const Slice& from, Slice& to);
/// @brief Function signature for move constructor/assignment.
using SliceMoveFn = void (*)(Slice&& from, Slice& to);
/// @brief Function signature for the destructor of a `Slice`.
using SliceDestructorFn = void (*)(Slice& slice);
/// @brief Function signature for retrieving the host pointer of a `Slice`.
using SliceHostPointerFn = void* (*)(Slice& slice);
/// @brief Function signature for a registered type's constructor, returning the type's own object.
using ParentConstructFn = void* (*)(ArgumentsHolder&& args);
/// @brief Function signature for the byte size of a registered type's object.
using ParentSizeFn = size_t (*)(void* parent);
/// @brief Function signature for the host pointer of a registered type's object.
using ParentHostPointerFn = void* (*)(void* parent);
/// @brief Function signature for destroying a registered type's object, or a payload of the type in a claim.
using ParentDestroyFn = void (*)(void* parent);
/** --------------------------------------------------------------------------------------------------------- SliceSemantics
 * @struct SliceSemantics
 * @brief Encapsulates the semantics for copying, moving, and destroying a memory slice, with the hooks of a
 * registered type that the registered behaviors call through to.
 */
struct SliceSemantics {
    /// @brief The word a claim of this table puts on its root, the index in the semantic bits; always first.
    uint32_t type = 0;
    /// @brief Indicates whether to use the built-in atomic reference counting for the memory slice.
    bool share = false;
    /// @brief Constructor function for the memory slice.
    SliceConstructFn construct_fn = nullptr;
    /// @brief Sub-slice function for the memory slice.
    SubSliceFn sub_slice_fn = nullptr;
    /// @brief Copy constructor function for the memory slice.
    SliceCopyFn copy_fn = nullptr;
    /// @brief Move constructor function for the memory slice.
    SliceMoveFn move_fn = nullptr;
    /// @brief Destructor function for the memory slice.
    SliceDestructorFn destructor_fn = nullptr;
    /// @brief Function to retrieve the host pointer for the memory slice.
    SliceHostPointerFn host_pointer_fn = nullptr;
    /// @brief A registered type's constructor of its object.
    ParentConstructFn parent_construct = nullptr;
    /// @brief A registered type's byte size of its object.
    ParentSizeFn parent_size = nullptr;
    /// @brief A registered type's host pointer of its object.
    ParentHostPointerFn parent_host_pointer = nullptr;
    /// @brief A registered type's destructor of its object, or of its payload in a claim.
    ParentDestroyFn parent_destroy = nullptr;
    /// @brief The placement a registered type's claims come from, null for a table nothing is claimed through.
    const BuffetDescriptor* placement = nullptr;
};
/** --------------------------------------------------------------------------------------------------------- Bitwidth Accessor (Static)
 * @brief Retrieves the Bitwidth in number of bits per element for the given type.
 * @param type The raw data byte from which to extract the bitwidth.
 * @return The Bitwidth in number of bits per element.
 */
inline static constexpr size_t bitwidth(uint32_t type) {
    return constants::BITWIDTHS[(type >> constants::BITWIDTH_BITS) & 0xF];
}
/** --------------------------------------------------------------------------------------------------------- Element Count Accessor
 * @brief Retrieves the ElementCount enum value from the raw data byte.
 * @param byte The raw data byte containing the element count information.
 * @return The ElementCount enum value corresponding to the element count.
 */
inline static constexpr size_t size(uint32_t type, uint32_t length) {
    return types::bitwidth(type) * static_cast<size_t>(length);
}
inline static constexpr size_t type_size_bits(uint32_t type, uint32_t length);
/** --------------------------------------------------------------------------------------------------------- Size (in bytes)
 * @brief Calculates the total size in bytes using the header-defined element count, if applicable.
 * @param bw The bitwidth of the elements.
 * @param ec The element count enumeration.
 * @param header_defined_element_count Optional header-defined element count.
 * @return The total size in bytes for the specified header-defined element
 * count.
 */
inline static constexpr size_t size_bytes(uint32_t type, uint32_t length) {
    return (types::type_size_bits(type, length) + 7ull) >> 3ull;
}
/** --------------------------------------------------------------------------------------------------------- Mask Of
 * @brief The bits of a type word that hold the given field.
 * @tparam T The field's enum type.
 * @return The field's mask.
 */
template<typename T>
inline static constexpr uint32_t mask_of() {
    if constexpr (std::is_same_v<T, Status>) return 1u << constants::STATUS_BIT;
    else if constexpr (std::is_same_v<T, State>) return 3u << constants::STATE_BITS;
    else if constexpr (std::is_same_v<T, PrimitiveType>) return 3u << constants::PRIMITIVE_BITS;
    else if constexpr (std::is_same_v<T, Structure>) return 3u << constants::STRUCTURE_BITS;
    else if constexpr (std::is_same_v<T, Alignment>) return 3u << constants::ALIGNMENT_BITS;
    else if constexpr (std::is_same_v<T, Ownership>) return 3u << constants::OWNERSHIP_BITS;
    else if constexpr (std::is_same_v<T, Location>) return 3u << constants::LOCATION_BITS;
    else if constexpr (std::is_same_v<T, Sync>) return 1u << constants::ATOMIC_BIT;
    else static_assert(always_false<T>::value, "Unsupported type for types::mask_of<T>");
}
/** --------------------------------------------------------------------------------------------------------- From uint32_t
 * @brief Converts the field of a type word to its enum type.
 * @tparam T The enum type to convert to.
 * @param value The type word.
 * @return The field's enum value.
 */
template<typename T>
inline static constexpr T from(uint32_t value) {
    return static_cast<T>(value & mask_of<T>());
}
/** --------------------------------------------------------------------------------------------------------- Size in Bits
 * @brief Calculates the total size in bits for the given bitwidth and element count.
 * @param bw The Bitwidth enum value.
 * @param ec The ElementCount enum value.
 * @param header_defined_element_count Optional header-defined element count.
 * @return The total size in bits for the specified bitwidth and element count.
 */
inline static constexpr size_t type_size_bits(uint32_t type, uint32_t length) {
    return bitwidth(type) * static_cast<size_t>(length);
}
} // namespace types
/** --------------------------------------------------------------------------------------------------------- SliceStatics
 * @struct SliceStatics
 * @brief The behaviors the semantic bits of a type word select: one table per kind of backing, and the registry
 * that hands out tables for registered types.
 */
struct SliceStatics {
    SliceStatics() = delete; // Prevent instantiation of this struct.
    /// @brief The tables seeded at startup, indices 0 to 4.
    inline static constexpr uint16_t BUILTIN_TABLES = 5;
    /// @brief Null: a zeroed Slice has nothing to count, and a weak view of a host address copies as fields.
    static Slice null_sub_slice(uint32_t type, void* parent, size_t offset, size_t length);
    static void null_copy(const Slice& from, Slice& to);
    static void null_move(Slice&& from, Slice& to);
    static void null_destroy(Slice& slice);
    static void* null_host_pointer(Slice& slice);
    /// @brief AlignedHeapBuffer: the root's parent is the buffer it alone owns.
    static Slice aligned_buffer_construct(uint32_t type, ArgumentsHolder&& args);
    static Slice aligned_buffer_sub_slice(uint32_t type, void* parent, size_t offset, size_t length);
    static void aligned_buffer_copy(const Slice& from, Slice& to);
    static void aligned_buffer_move(Slice&& from, Slice& to);
    static void aligned_buffer_destroy(Slice& slice);
    static void* aligned_buffer_host_pointer(Slice& slice);
    /// @brief VulkanBuffer: the root's parent is the mapped buffer it alone owns.
    static Slice vulkan_buffer_construct(uint32_t type, ArgumentsHolder&& args);
    static Slice vulkan_buffer_sub_slice(uint32_t type, void* parent, size_t offset, size_t length);
    static void vulkan_buffer_copy(const Slice& from, Slice& to);
    static void vulkan_buffer_move(Slice&& from, Slice& to);
    static void vulkan_buffer_destroy(Slice& slice);
    static void* vulkan_buffer_host_pointer(Slice& slice);
    /// @brief Subslice: a handle, whose parent is the root it counts on; every Slice a caller holds is one.
    static Slice subslice_construct(uint32_t type, ArgumentsHolder&& args);
    static Slice subslice_sub_slice(uint32_t type, void* parent, size_t offset, size_t length);
    static void subslice_copy(const Slice& from, Slice& to);
    static void subslice_move(Slice&& from, Slice& to);
    static void subslice_destroy(Slice& slice);
    static void* subslice_host_pointer(Slice& slice);
    /// @brief ChainBuffet: the root sits in the first line of its claim and its parent is the link.
    static Slice chain_buffer_construct(uint32_t type, ArgumentsHolder&& args);
    static Slice chain_buffer_sub_slice(uint32_t type, void* parent, size_t offset, size_t length);
    static void chain_buffer_copy(const Slice& from, Slice& to);
    static void chain_buffer_move(Slice&& from, Slice& to);
    static void chain_buffer_destroy(Slice& slice);
    static void* chain_buffer_host_pointer(Slice& slice);
    /// @brief Registered: the root's parent is the object the type's own hooks make, measure and destroy.
    static Slice registered_construct(uint32_t type, ArgumentsHolder&& args);
    static Slice registered_sub_slice(uint32_t type, void* parent, size_t offset, size_t length);
    static void registered_copy(const Slice& from, Slice& to);
    static void registered_move(Slice&& from, Slice& to);
    static void registered_destroy(Slice& slice);
    static void* registered_host_pointer(Slice& slice);
    /// @brief A chain root holding a registered type's payload: its destructor runs ahead of the link release.
    static void payload_destroy(Slice& slice);
    /// @brief Semantics of the null Slice.
    inline static constexpr types::SliceSemantics NULL_SEMANTICS = {
        .type = 0,
        .share = false,
        .construct_fn = nullptr,
        .sub_slice_fn = null_sub_slice,
        .copy_fn = null_copy,
        .move_fn = null_move,
        .destructor_fn = null_destroy,
        .host_pointer_fn = null_host_pointer
    };
    /// @brief Semantics for an aligned buffer slice.
    inline static constexpr types::SliceSemantics ALIGNED_BUFFER_SEMANTICS = {
        .type = static_cast<uint32_t>(types::Status::Valid) | static_cast<uint32_t>(types::Alignment::VectorAligned)
            | static_cast<uint32_t>(types::Ownership::Owned) | static_cast<uint32_t>(types::Location::Heap),
        .share = true,
        .construct_fn = aligned_buffer_construct,
        .sub_slice_fn = aligned_buffer_sub_slice,
        .copy_fn = aligned_buffer_copy,
        .move_fn = aligned_buffer_move,
        .destructor_fn = aligned_buffer_destroy,
        .host_pointer_fn = aligned_buffer_host_pointer
    };
    /// @brief Semantics for a Vulkan buffer slice.
    inline static constexpr types::SliceSemantics VULKAN_BUFFER_SEMANTICS = {
        .type = static_cast<uint32_t>(types::Status::Valid) | static_cast<uint32_t>(types::Alignment::VectorAligned)
            | static_cast<uint32_t>(types::Ownership::Owned) | static_cast<uint32_t>(types::Location::HostCoherent),
        .share = true,
        .construct_fn = vulkan_buffer_construct,
        .sub_slice_fn = vulkan_buffer_sub_slice,
        .copy_fn = vulkan_buffer_copy,
        .move_fn = vulkan_buffer_move,
        .destructor_fn = vulkan_buffer_destroy,
        .host_pointer_fn = vulkan_buffer_host_pointer
    };
    /// @brief Semantics for a subslice.
    inline static constexpr types::SliceSemantics SUBSLICE_SEMANTICS = {
        .type = static_cast<uint32_t>(types::Status::Valid) | static_cast<uint32_t>(types::Ownership::Shared),
        .share = true,
        .construct_fn = subslice_construct,
        .sub_slice_fn = subslice_sub_slice,
        .copy_fn = subslice_copy,
        .move_fn = subslice_move,
        .destructor_fn = subslice_destroy,
        .host_pointer_fn = subslice_host_pointer
    };
    /// @brief Semantics for a chain buffer slice.
    inline static constexpr types::SliceSemantics CHAIN_BUFFER_SEMANTICS = {
        .type = static_cast<uint32_t>(types::Status::Valid) | static_cast<uint32_t>(types::Alignment::VectorAligned)
            | static_cast<uint32_t>(types::Ownership::Pooled),
        .share = true,
        .construct_fn = chain_buffer_construct,
        .sub_slice_fn = chain_buffer_sub_slice,
        .copy_fn = chain_buffer_copy,
        .move_fn = chain_buffer_move,
        .destructor_fn = chain_buffer_destroy,
        .host_pointer_fn = chain_buffer_host_pointer
    };
    /// @brief Semantics a registered type's table starts from: a root over the type's own object.
    inline static constexpr types::SliceSemantics REGISTERED_SEMANTICS = {
        .type = static_cast<uint32_t>(types::Status::Valid) | static_cast<uint32_t>(types::Ownership::Owned)
            | static_cast<uint32_t>(types::Location::Heap),
        .share = true,
        .construct_fn = registered_construct,
        .sub_slice_fn = registered_sub_slice,
        .copy_fn = registered_copy,
        .move_fn = registered_move,
        .destructor_fn = registered_destroy,
        .host_pointer_fn = registered_host_pointer
    };
    /** ------------------------------------------------------------------------------------------- Semantics Initializer
     * @brief Initializes the array of slice semantics with default values.
     * @return The initialized array of slice semantics.
     */
    inline static std::array<std::pair<types::SliceSemantics, const std::type_info*>, 4096> semantics_initializer() {
        std::array<std::pair<types::SliceSemantics, const std::type_info*>, 4096> array{};
        array[0] = {SliceStatics::NULL_SEMANTICS, &typeid(std::nullptr_t)};
        array[1] = {SliceStatics::ALIGNED_BUFFER_SEMANTICS, &typeid(AlignedHeapBuffer)};
        array[2] = {SliceStatics::VULKAN_BUFFER_SEMANTICS, &typeid(VulkanBuffer)};
        array[3] = {SliceStatics::SUBSLICE_SEMANTICS, &typeid(Slice)};
        array[4] = {SliceStatics::CHAIN_BUFFER_SEMANTICS, &typeid(ChainBuffet)};
        /// A table's slot is its index, stamped into its type word here as add_semantics stamps the rest.
        for (uint16_t index = 0; index < BUILTIN_TABLES; ++index) {
            array[index].first.type |= uint32_t{index} << types::constants::SEMANTIC_BITS;
        }
        return array;
    }
    /** ------------------------------------------------------------------------------------------- Next Type Index
     * @brief Returns a reference to the next available type index, used for semantic behavior registrations.
     * @return Reference to the next available type index.
     */
    static std::atomic<uint16_t>& next_type_index() {
        static std::atomic<uint16_t> instance{BUILTIN_TABLES};
        return instance;
    }
    /** ------------------------------------------------------------------------------------------- Slice Semantics Array
     * @brief Returns the static array of slice semantics paired with their parent types, built once so no guard
     * sits on the dispatch path.
     * @return The array of slice semantics.
     */
    static void* semantics() {
        static std::array<std::pair<types::SliceSemantics, const std::type_info*>, 4096> instance =
            SliceStatics::semantics_initializer();
        return static_cast<void*>(instance.data());
    }
    /** ------------------------------------------------------------------------------------------- Semantics Accessor
     * @brief Retrieves the slice semantics for the given type.
     * @param type The type for which to retrieve the semantics.
     * @return Reference to the corresponding slice semantics.
     */
    static types::SliceSemantics& semantics(uint32_t type) {
        return static_cast<std::pair<types::SliceSemantics, const std::type_info*>*>(
            SliceStatics::semantics()
        )[(type >> types::constants::SEMANTIC_BITS) & ((1 << (32 - types::constants::SEMANTIC_BITS)) - 1)].first;
    }
    /** ------------------------------------------------------------------------------------------- Parent Type Of
     * @brief Retrieves the parent type a Slice of the given type word points at, as its table was registered.
     * @param type The type word.
     * @return The parent type.
     */
    static const std::type_info& parent_type_of(uint32_t type) {
        return *static_cast<std::pair<types::SliceSemantics, const std::type_info*>*>(
            SliceStatics::semantics()
        )[(type >> types::constants::SEMANTIC_BITS) & ((1 << (32 - types::constants::SEMANTIC_BITS)) - 1)].second;
    }
    /** ------------------------------------------------------------------------------------------- Semantics Of
     * @brief Retrieves the slice semantics registered for a type.
     * @param type The registered type.
     * @return Reference to the corresponding slice semantics.
     */
    static types::SliceSemantics& semantics_of(const std::type_info& type) {
        std::shared_lock lock(type_index_mutex());
        return semantics(uint32_t{type_index_map().at(type.hash_code())} << types::constants::SEMANTIC_BITS);
    }
    /** ------------------------------------------------------------------------------------------- Seeded Type Index Map
     * @brief The built-in tables' type hashes mapped to their indices, read from the tables themselves.
     * @return The seeded map.
     */
    static absl::flat_hash_map<size_t, uint16_t> seeded_type_index_map();
    /** ------------------------------------------------------------------------------------------- Type Index Map
     * @brief Returns a reference to the static type index map.
     * @return Reference to the type index map.
     */
    static absl::flat_hash_map<size_t, uint16_t>& type_index_map() {
        static absl::flat_hash_map<size_t, uint16_t> instance = SliceStatics::seeded_type_index_map();
        return instance;
    }
    /** ------------------------------------------------------------------------------------------- Type Index Mutex
     * @brief Returns a reference to the static type index mutex.
     * @return Reference to the type index mutex.
     */
    static std::shared_mutex& type_index_mutex() {
        static std::shared_mutex instance;
        return instance;
    }
    /** ------------------------------------------------------------------------------------------- Add Semantics
     * @brief Adds a table and returns its index, stamped into the table's type word and mapped from the type.
     * @param table The table to add.
     * @param type_info The registered type.
     * @return The index of the newly added table.
     */
    static uint16_t add_semantics(types::SliceSemantics table, const std::type_info* type_info) {
        const uint16_t index = next_type_index().fetch_add(1, std::memory_order_relaxed);
        table.type = (table.type & ((1u << types::constants::SEMANTIC_BITS) - 1u))
            | (uint32_t{index} << types::constants::SEMANTIC_BITS);
        static_cast<std::pair<types::SliceSemantics, const std::type_info*>*>(semantics())[index] = {table, type_info};
        std::unique_lock lock(type_index_mutex());
        type_index_map()[type_info->hash_code()] = index;
        return index;
    }
    /** ------------------------------------------------------------------------------------------- Type Emulator
     * @brief Returns the type emulator for the given type.
     * @param type The type word.
     * @param num_elements The number of elements for the type emulator.
     * @return The type emulator.
     */
    static const Slice* type_emulator(uint32_t type, uint32_t num_elements) {
        return Slice::type_emulator(type, num_elements);
    }
};
} // namespace buffetalligator