#pragma once
/** --------------------------------------------------------------------------------------------------------- EasyMmap
 * @file easymmap.hpp
 * @brief The mmap scratch-file buffet type: zero-filled MAP_SHARED mappings of unlinked files in a
 * caller-chosen directory, for disk- or RAM-filesystem-backed Slices.
 */
#include <alligator.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- MmapBuffer
 * @class MmapBuffer
 * @brief One scratch-file mapping in the registered directory; the mmap buffet type.
 */
class alignas(16) MmapBuffer {
private:
    void* mapping_ = nullptr;   ///< The MAP_SHARED mapping of the unlinked scratch file.
    size_t size_ = 0;           ///< Size in bytes.
    int descriptor_ = -1;       ///< The scratch file's descriptor, open for the mapping's lifetime.
    /** ------------------------------------------------------------------------------------------- release
     * @brief Unmaps and closes the scratch file; defined in mmap_allocator.cpp.
     */
    void release();
    /** ------------------------------------------------------------------------------------------- Buffer For
     * @brief The MmapBuffer backing a Slice claimed from this type.
     * @param slice The Slice.
     * @return Its backing buffer.
     */
    static const MmapBuffer& buffer_for(const Slice& slice);
public:
    MmapBuffer() = default;
    /** ------------------------------------------------------------------------------------------- Allocating Constructor
     * @brief Creates, reserves, unlinks, and maps one zero-filled scratch file.
     * @param size Buffer size in bytes, rounded up to 64.
     */
    explicit MmapBuffer(size_t size);
    /** ------------------------------------------------------------------------------------------- Move-only ownership */
    MmapBuffer(const MmapBuffer&) = delete;
    MmapBuffer& operator=(const MmapBuffer&) = delete;
    MmapBuffer(MmapBuffer&& other) noexcept
    : mapping_(std::exchange(other.mapping_, nullptr)), size_(std::exchange(other.size_, 0))
    , descriptor_(std::exchange(other.descriptor_, -1)) {}
    MmapBuffer& operator=(MmapBuffer&& other) noexcept {
        std::swap(mapping_, other.mapping_); std::swap(size_, other.size_);
        std::swap(descriptor_, other.descriptor_); return *this;
    }
    ~MmapBuffer() { release(); }
    operator bool() const { return mapping_ != nullptr; }
    bool operator==(std::nullptr_t) const { return mapping_ == nullptr; }
    bool operator!=(std::nullptr_t) const { return mapping_ != nullptr; }
    /** ------------------------------------------------------------------------------------------- Buffet Hooks
     * @brief The static hooks BuffetDescriptors::descriptor_for binds for this type.
     */
    static void deleter_impl(void* ptr) { delete static_cast<MmapBuffer*>(ptr); }
    static void (*deleter())(void*) { return deleter_impl; }
    static void* host_ptr_impl(void* ptr, size_t offset) {
        return static_cast<uint8_t*>(static_cast<MmapBuffer*>(ptr)->mapping_) + offset;
    }
    static void* host_ptr(void* ptr, size_t offset) { return host_ptr_impl(ptr, offset); }
    static void* (*host_ptr())(void*, size_t) { return host_ptr_impl; }
    static size_t size_of_impl(void* ptr) { return static_cast<MmapBuffer*>(ptr)->size_; }
    static size_t size_of(void* ptr) { return size_of_impl(ptr); }
    static size_t (*size_of())(void*) { return size_of_impl; }
    static void* factory(size_t size) { return new MmapBuffer(size); }
    static size_t default_size() { return 64 * 1024 * 1024; }
    static size_t type_idx() { return 3; }
    static const char* type_name() { return "MmapBuffer"; }
    static uint64_t device_address(void*) { return 0; }
    size_t size() const { return size_; }
    void* raw() { return mapping_; }
    const void* raw() const { return mapping_; }
    /** ------------------------------------------------------------------------------------------- Register Type
     * @brief Checks the scratch directory and registers the mmap buffet descriptor at type index 3.
     * @param directory A directory on the filesystem that should back the scratch files.
     * @return The registered descriptor.
     */
    static const BuffetDescriptor* register_type(const std::string& directory);
    /** ------------------------------------------------------------------------------------------- Flush
     * @brief Synchronously writes a Slice's page-aligned mapped range back to its scratch file.
     * @param slice A Slice claimed from this type.
     */
    static void flush(const Slice& slice);
    /** ------------------------------------------------------------------------------------------- File Descriptor
     * @brief The borrowed descriptor of the scratch file behind a Slice.
     * @param slice A Slice claimed from this type.
     * @return The descriptor, valid while the Slice lives.
     */
    static int file_descriptor(const Slice& slice);
    /** ------------------------------------------------------------------------------------------- File Offset
     * @brief A Slice's byte offset within its scratch file, including sub-slice offsets.
     * @param slice A Slice claimed from this type.
     * @return The byte offset.
     */
    static uint64_t file_offset(const Slice& slice);
};
static_assert(IsABuffetType<MmapBuffer>, "MmapBuffer must satisfy IsABuffetType");
} // namespace buffetalligator
