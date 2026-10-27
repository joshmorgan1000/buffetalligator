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
class alignas(16) MmapBuffet {
private:
    void* mapping_ = nullptr;   ///< The MAP_SHARED mapping of the unlinked scratch file.
    size_t size_ = 0;           ///< Size in bytes.
    int descriptor_ = -1;       ///< The scratch file's descriptor, open for the mapping's lifetime.
    /** ------------------------------------------------------------------------------------------- release
     * @brief Unmaps and closes the scratch file; defined in mmap_allocator.cpp.
     */
    void release();
public:
    MmapBuffet() = default;
    /** ------------------------------------------------------------------------------------------- Allocating Constructor
     * @brief Creates, reserves, unlinks, and maps one zero-filled scratch file.
     * @param size Buffer size in bytes, rounded up to 64.
     */
    explicit MmapBuffet(size_t size);
    /** ------------------------------------------------------------------------------------------- Move-only ownership */
    MmapBuffet(const MmapBuffet&) = delete;
    MmapBuffet& operator=(const MmapBuffet&) = delete;
    MmapBuffet(MmapBuffet&& other) noexcept
    : mapping_(std::exchange(other.mapping_, nullptr)), size_(std::exchange(other.size_, 0))
    , descriptor_(std::exchange(other.descriptor_, -1)) {}
    MmapBuffet& operator=(MmapBuffet&& other) noexcept {
        std::swap(mapping_, other.mapping_); std::swap(size_, other.size_);
        std::swap(descriptor_, other.descriptor_); return *this;
    }
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Releases the memory-mapped buffer by unmapping and closing the associated file.
     */
    ~MmapBuffet() { release(); }
    operator bool() const { return mapping_ != nullptr; }
    bool operator==(std::nullptr_t) const { return mapping_ == nullptr; }
    bool operator!=(std::nullptr_t) const { return mapping_ != nullptr; }
    /** ------------------------------------------------------------------------------------------- Buffet Hooks
     * @brief The static hooks BuffetDescriptors::descriptor_for binds for this type.
     */
    static void deleter_impl(void* ptr) { delete static_cast<MmapBuffet*>(ptr); }
    static void (*deleter())(void*) { return deleter_impl; }
    static void* host_ptr_impl(void* ptr) {
        return static_cast<uint8_t*>(static_cast<MmapBuffet*>(ptr)->mapping_);
    }
    static void* host_ptr(void* ptr) { return host_ptr_impl(ptr); }
    static void* (*host_ptr())(void*) { return host_ptr_impl; }
    static size_t size_of_impl(void* ptr) { return static_cast<MmapBuffet*>(ptr)->size_; }
    static size_t size_of(void* ptr) { return size_of_impl(ptr); }
    static size_t (*size_of())(void*) { return size_of_impl; }
    static void* factory(size_t size) { return new MmapBuffet(size); }
    static size_t default_size() { return 256 * 1024 * 1024; }
    static size_t* type_idx() { static size_t idx = 3; return &idx; }
    static const char* type_name() { return "Memory Mapped File"; }
    static uint64_t device_address(void*) { return 0; }
    size_t size() const { return size_; }
    /** ------------------------------------------------------------------------------------------- Raw Pointer Accessor
     * @brief Returns a raw pointer to the memory-mapped buffer.
     * @return A raw pointer to the memory-mapped buffer.
     */
    void* raw() { return mapping_; }
    /** ------------------------------------------------------------------------------------------- Raw Pointer Accessor
     * @brief Returns a raw pointer to the memory-mapped buffer.
     * @return A raw pointer to the memory-mapped buffer.
     */
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
static_assert(IsABuffetType<MmapBuffet>, "MmapBuffet must satisfy IsABuffetType");
} // namespace buffetalligator
