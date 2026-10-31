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
/** --------------------------------------------------------------------------------------------------------- Slice Constructor
 * @brief Constructs a slice with the given size and novelty.
 * @param size The size of the slice in bytes.
 * @param novel Whether the slice should be backed by a novel buffer.
 */
Slice::Slice(size_t size, bool novel) { *this = IBuffet::claim(static_cast<uint32_t>(size), novel); }
/** --------------------------------------------------------------------------------------------------------- Slice Constructor
 * @brief Constructs a slice with the given data, size, and novelty.
 * @param data Pointer to the data to initialize the slice with.
 * @param size The size of the slice in bytes.
 * @param novel Whether the slice should be backed by a novel buffer.
 */
Slice::Slice(const void* data, size_t size, bool novel) {
    *this = IBuffet::claim(static_cast<uint32_t>(size), novel);
    if (size != 0) std::memcpy(raw(), data, size);
}
/** --------------------------------------------------------------------------------------------------------- Slice Copy Constructor
 * @brief Constructs a slice as a copy of another slice.
 * @param other The slice to copy.
 */
Slice::Slice(const Slice& other)
: id_(other.id_ == 0xFFFFFFFFu ? 0xFFFFFFFFu : IBuffet::copy_rec(other.rec(), other.address())) {}
/** --------------------------------------------------------------------------------------------------------- Slice Copy Assignment
 * @brief Assigns the slice as a copy of another slice.
 * @param other The slice to copy.
 */
Slice& Slice::operator=(const Slice& other) {
    if (this != &other) {
        if (id_ != 0xFFFFFFFFu) rec().release();
        id_ = other.id_ == 0xFFFFFFFFu ? 0xFFFFFFFFu : IBuffet::copy_rec(other.rec(), other.address());
    }
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Slice Move Constructor
 * @brief Constructs a slice by moving another slice.
 * @param other The slice to move.
 */
Slice::Slice(Slice&& other) : id_(other.id_) { other.id_ = 0xFFFFFFFFu; }
/** --------------------------------------------------------------------------------------------------------- Slice Move Assignment
 * @brief Assigns the slice by moving another slice.
 * @param other The slice to move.
 */
Slice& Slice::operator=(Slice&& other) {
    if (this != &other) {
        if (id_ != 0xFFFFFFFFu) rec().release();
        id_ = other.id_;
        other.id_ = 0xFFFFFFFFu;
    }
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Slice Destructor
 * @brief Releases the resources held by the slice.
 */
Slice::~Slice() { if (id_ != 0xFFFFFFFFu) rec().release(); }
/** --------------------------------------------------------------------------------------------------------- Slice::free
 * @brief Frees the resources held by the slice and resets its ID.
 */
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
        rec().size_ = new_size;
        return;
    }
    Slice fresh = IBuffet::claim(static_cast<uint32_t>(new_size), novel_buffer);
    if (preserve_data && id_ != 0xFFFFFFFFu && new_size != 0) {
        std::memcpy(fresh.raw(), raw(), std::min(size_bytes(), new_size));
    }
    *this = std::move(fresh);
}
/** --------------------------------------------------------------------------------------------------------- Slice::raw
 * @brief Returns the raw pointer to the slice data.
 * @return Pointer to the slice data.
 */
void* Slice::raw() { return rec().raw_; }
/** --------------------------------------------------------------------------------------------------------- Slice::size_bytes
 * @brief Returns the size of the slice in bytes.
 * @return Size of the slice in bytes.
 */
const void* Slice::raw() const { return rec().raw_; }
/** --------------------------------------------------------------------------------------------------------- Slice::size_bytes
 * @brief Returns the size of the slice in bytes.
 * @return Size of the slice in bytes.
 */
size_t Slice::size_bytes() const { return rec().size_; }
/** --------------------------------------------------------------------------------------------------------- Slice::slice
 * @brief Creates a sub-slice of the current slice.
 * @param offset Offset into the current slice.
 * @param length Length of the new slice.
 * @return A new Slice representing the sub-slice.
 */
Slice Slice::slice(size_t offset, size_t length) const {
    return Slice(*rec().sub_slice(offset, length));
}
/** --------------------------------------------------------------------------------------------------------- Slice::buffer
 * @brief Returns the shared pointer to the associated buffer.
 * @return Shared pointer to the buffer.
 */
std::shared_ptr<IBuffet> Slice::buffer() const { return rec().buffer_; }
/** --------------------------------------------------------------------------------------------------------- Slice::rec
 * @brief Returns the underlying SliceRec.
 * @return Reference to the SliceRec.
 */
SliceRec& Slice::rec() { return *IBuffet::ref(id_); }
/** --------------------------------------------------------------------------------------------------------- Slice::address
 * @brief Returns the address of the slice.
 * @return Reference to the address.
 */
const SliceRec& Slice::rec() const { return *IBuffet::ref(id_); }
/** --------------------------------------------------------------------------------------------------------- Slice::address
 * @brief Returns the address of the slice.
 * @return Reference to the address.
 */
uint64_t& Slice::address() { return IBuffet::addresses()[id_ & 0xFFFFFu]; }
/** --------------------------------------------------------------------------------------------------------- Slice::address
 * @brief Returns the address of the slice.
 * @return Reference to the address.
 */
const uint64_t& Slice::address() const { return IBuffet::addresses()[id_ & 0xFFFFFu]; }
/** --------------------------------------------------------------------------------------------------------- Slice::Slice
 * @brief Constructs a Slice from a SliceRec.
 * @param rec The SliceRec to construct from.
 */
Slice::Slice(const SliceRec& rec)
: id_(static_cast<uint32_t>(&rec - static_cast<const SliceRec*>(IBuffet::pool()))) {}
/** --------------------------------------------------------------------------------------------------------- Slice::Slice
 * @brief Constructs a Slice from its components.
 * @param address The address of the slice.
 * @param data Pointer to the slice data.
 * @param id The ID of the slice.
 * @param flags Additional flags for the slice.
 */
Slice::Slice(uint64_t, void*, uint32_t id, uint32_t) : id_(id) {}
} // namespace buffetalligator
