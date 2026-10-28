/** --------------------------------------------------------------------------------------------------------- BuffetAlligator Memory
 * @file slice.cpp
 * @brief The buffer behind every claim and the Slice handle over it: one definition each, so every consumer of
 * the public header links against these.
 */
#include <alligator.hpp>
#include <gpu/easyvulkan.hpp>
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- IBuffet Constructor
 * @brief One buffer: the five Vulkan calls against the cached statics when a device is present, an aligned
 * heap block otherwise; either way the mapping and its address are what a claim cuts from.
 * @param size Bytes.
 * @param novel Whether the buffer is dedicated to one claim.
 */
IBuffet::IBuffet(size_t size, bool novel) : size_(static_cast<uint32_t>(size)), novel_(novel) {
    live_.fetch_add(1, std::memory_order_acq_rel);
    auto& statics = VulkanBuffer::statics();
    if (statics.device) {
        const vk::Buffer buffer = statics.device.createBuffer(
            size == 64 * 1024 * 1024 ? statics.create_info
                : vk::BufferCreateInfo({}, size, statics.usage, vk::SharingMode::eExclusive)
        );
        const vk::DeviceMemory memory = statics.device.allocateMemory(
            size == 64 * 1024 * 1024 ? statics.alloc_info64mb
                : statics.alloc_info_custom(size)
        );
        statics.device.bindBufferMemory(buffer, memory, 0);
        mapped_ = statics.device.mapMemory(memory, 0, size);
        address_ = statics.device.getBufferAddress(vk::BufferDeviceAddressInfo(buffer));
        buffer_ = static_cast<VkBuffer>(buffer);
        memory_ = static_cast<VkDeviceMemory>(memory);
    } else {
        const size_t rounded = (size + 63) & ~size_t{63};
        mapped_ = std::aligned_alloc(64, rounded);
        if (mapped_ == nullptr) ALLIGATOR_THROW("Allocating " + std::to_string(rounded) + " heap bytes failed");
        std::memset(mapped_, 0, rounded);
        address_ = reinterpret_cast<uint64_t>(mapped_);
    }
}
/** --------------------------------------------------------------------------------------------------------- IBuffet Destructor
 * @brief Frees the mapping the way it was made; nothing touches a device that is already gone.
 */
IBuffet::~IBuffet() {
    live_.fetch_sub(1, std::memory_order_acq_rel);
    if (finalize_) finalize_(mapped_);
    auto& statics = VulkanBuffer::statics();
    if (buffer_ != nullptr) {
        if (statics.stopped) return;
        const vk::DeviceMemory memory(static_cast<VkDeviceMemory>(memory_));
        statics.device.unmapMemory(memory);
        statics.device.freeMemory(memory);
        statics.device.destroyBuffer(vk::Buffer(static_cast<VkBuffer>(buffer_)));
    } else {
        std::free(mapped_);
    }
    if (id_ != 0xFFFFFFFFu && !fast_allocator().stopping) fast_allocator().addresses[id_] = 0;
}
Slice::Slice(size_t size, bool novel) { *this = IBuffet::claim(static_cast<uint32_t>(size), novel); }
Slice::Slice(const void* data, size_t size, bool novel) {
    *this = IBuffet::claim(static_cast<uint32_t>(size), novel);
    if (size != 0) std::memcpy(raw(), data, size);
}
Slice::Slice(const Slice& other)
: id_(other.id_ == 0xFFFFFFFFu ? 0xFFFFFFFFu : IBuffet::copy_rec(other.rec(), other.address())) {}
Slice& Slice::operator=(const Slice& other) {
    if (this != &other) {
        if (id_ != 0xFFFFFFFFu) rec().release();
        id_ = other.id_ == 0xFFFFFFFFu ? 0xFFFFFFFFu : IBuffet::copy_rec(other.rec(), other.address());
    }
    return *this;
}
Slice::Slice(Slice&& other) : id_(other.id_) { other.id_ = 0xFFFFFFFFu; }
Slice& Slice::operator=(Slice&& other) {
    if (this != &other) {
        if (id_ != 0xFFFFFFFFu) rec().release();
        id_ = other.id_;
        other.id_ = 0xFFFFFFFFu;
    }
    return *this;
}
Slice::~Slice() { if (id_ != 0xFFFFFFFFu) rec().release(); }
void Slice::free() {
    if (id_ != 0xFFFFFFFFu) rec().release();
    id_ = 0xFFFFFFFFu;
}
/** --------------------------------------------------------------------------------------------------------- Slice::resize
 * @brief Resizes the slice to the new size.
 * @param new_size The new size of the slice.
 * @param preserve_data Whether to preserve the existing data.
 * @param novel_buffer Whether to allocate a novel buffer.
 */
void Slice::resize(size_t new_size, bool preserve_data, bool novel_buffer) {
    if (preserve_data && id_ != 0xFFFFFFFFu && new_size != 0 && new_size <= size_bytes()) {
        rec().size_ = static_cast<uint32_t>(new_size);
        return;
    }
    Slice fresh = IBuffet::claim(static_cast<uint32_t>(new_size), novel_buffer);
    if (preserve_data && id_ != 0xFFFFFFFFu && new_size != 0) {
        std::memcpy(fresh.raw(), raw(), std::min(size_bytes(), new_size));
    }
    *this = std::move(fresh);
}
void* Slice::raw() { return rec().raw_; }
const void* Slice::raw() const { return rec().raw_; }
size_t Slice::size_bytes() const { return rec().size_; }
Slice Slice::slice(size_t offset, size_t length) const {
    return Slice(*IBuffet::ref(IBuffet::copy_rec(rec().slice(offset, length), address() + offset)));
}
std::shared_ptr<IBuffet> Slice::buffer() const { return rec().buffer_; }
bool Slice::is_novel() const { return id_ != 0xFFFFFFFFu && rec().buffer_ && rec().buffer_->novel_; }
SliceRec& Slice::rec() { return *IBuffet::ref(id_); }
const SliceRec& Slice::rec() const { return *IBuffet::ref(id_); }
uint64_t& Slice::address() { return IBuffet::addresses()[id_ & 0xFFFFFu]; }
const uint64_t& Slice::address() const { return IBuffet::addresses()[id_ & 0xFFFFFu]; }
Slice::Slice(const SliceRec& rec)
: id_(static_cast<uint32_t>(&rec - static_cast<const SliceRec*>(IBuffet::pool()))) {}
Slice::Slice(uint64_t, void*, uint32_t id, uint32_t) : id_(id) {}
} // namespace buffetalligator
