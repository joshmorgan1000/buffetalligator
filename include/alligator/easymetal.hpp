#pragma once
/** --------------------------------------------------------------------------------------------------------- EasyMetal
 * @file easymetal.hpp
 * @brief Exposes registered shared Metal storage while native shader execution remains private.
 */
#if defined(BUFFETALLIGATOR_HAS_METAL)
#include <alligator.hpp>
#include <alligator/easygpu.hpp>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- MetalBuffer
 * @class MetalBuffer
 * @brief One zero-initialized shared MTLBuffer on the registered device; the Metal buffet type.
 */
class alignas(16) MetalBuffer {
private:
    void* buffer_ = nullptr;    ///< Retained id<MTLBuffer>, bridged.
    void* host_ = nullptr;      ///< The buffer's persistent contents pointer.
    uint64_t address_ = 0;      ///< The buffer's gpuAddress.
    size_t size_ = 0;           ///< Size in bytes.
    /** ------------------------------------------------------------------------------------------- release
     * @brief Removes the retired allocation from residency and clears its native address and size.
     */
    void release();
public:
    MetalBuffer() = default;
    /** ------------------------------------------------------------------------------------------- Allocating Constructor
     * @brief Creates one zero-initialized shared MTLBuffer; defined in metal_allocator.mm.
     * @param size Buffer size in bytes, rounded up to 64.
     */
    explicit MetalBuffer(size_t size);
    /** ------------------------------------------------------------------------------------------- Move-only ownership */
    MetalBuffer(const MetalBuffer&) = delete;
    MetalBuffer& operator=(const MetalBuffer&) = delete;
    MetalBuffer(MetalBuffer&& other) noexcept
    : buffer_(std::exchange(other.buffer_, nullptr)), host_(std::exchange(other.host_, nullptr))
    , address_(std::exchange(other.address_, 0)), size_(std::exchange(other.size_, 0)) {}
    MetalBuffer& operator=(MetalBuffer&& other) noexcept {
        std::swap(buffer_, other.buffer_); std::swap(host_, other.host_);
        std::swap(address_, other.address_); std::swap(size_, other.size_); return *this;
    }
    ~MetalBuffer() { release(); }
    operator bool() const { return host_ != nullptr; }
    bool operator==(std::nullptr_t) const { return host_ == nullptr; }
    bool operator!=(std::nullptr_t) const { return host_ != nullptr; }
    /** ------------------------------------------------------------------------------------------- Buffet Hooks
     * @brief The static hooks BuffetDescriptors::descriptor_for binds for this type.
     */
    static void deleter_impl(void* ptr) { delete static_cast<MetalBuffer*>(ptr); }
    static void (*deleter())(void*) { return deleter_impl; }
    static void* host_ptr_impl(void* ptr, size_t offset) {
        return static_cast<uint8_t*>(static_cast<MetalBuffer*>(ptr)->host_) + offset;
    }
    static void* host_ptr(void* ptr, size_t offset) { return host_ptr_impl(ptr, offset); }
    static void* (*host_ptr())(void*, size_t) { return host_ptr_impl; }
    static size_t size_of_impl(void* ptr) { return static_cast<MetalBuffer*>(ptr)->size_; }
    static size_t size_of(void* ptr) { return size_of_impl(ptr); }
    static size_t (*size_of())(void*) { return size_of_impl; }
    static void* factory(size_t size) { return new MetalBuffer(size); }
    static size_t default_size() { return 64 * 1024 * 1024; }
    static size_t type_idx() { return 2; }
    static const char* type_name() { return "MetalBuffer"; }
    static uint64_t device_address(void* ptr) { return static_cast<MetalBuffer*>(ptr)->address_; }
    size_t size() const { return size_; }
    void* raw() { return host_; }
    const void* raw() const { return host_; }
    /** ------------------------------------------------------------------------------------------- Buffer
     * @brief The borrowed id<MTLBuffer>, bridged, for encoder binding.
     * @return The native buffer.
     */
    void* buffer() const { return buffer_; }
    /** ------------------------------------------------------------------------------------------- Register Type
     * @brief Retains the caller's device, probes its limits, and registers the Metal buffet
     * descriptor at type index 2.
     * @param device A bridged id<MTLDevice>.
     * @return The registered descriptor.
     */
    static const BuffetDescriptor* register_type(void* device);
    /** ------------------------------------------------------------------------------------------- Memory Usage
     * @brief Reports currentAllocatedSize and recommendedMaxWorkingSetSize for the device.
     * @return The device's memory usage.
     */
    static DeviceMemoryUsage memory_usage();
};
static_assert(IsABuffetType<MetalBuffer>, "MetalBuffer must satisfy IsABuffetType");
} // namespace buffetalligator
#endif
