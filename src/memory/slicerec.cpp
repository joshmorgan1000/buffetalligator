/** --------------------------------------------------------------------------------------------------------- SliceRec Implementation
 * @file slicerec.cpp
 * @brief Implementation of the SliceRec class and related functions.
 */
#include <logging.hpp>
#include <alligator.hpp>

namespace buffetalligator {
namespace {
} // anonymous namespace
/** --------------------------------------------------------------------------------------------------------- SliceRec
 * @brief Creates a sub-slice of the current SliceRec.
 * @param offset Offset into the current slice.
 * @param length Length of the new slice.
 * @return A new SliceRec representing the slice.
 */
SliceRec* SliceRec::sub_slice(size_t offset, size_t length) const {
    if (offset > size_.size()) return nullptr;
    if (length == SIZE_MAX) length = size_.size() - offset;
    SliceRec* slice = IBuffet::next_available();
    slice->buffer_ = buffer_;
    slice->raw_ = static_cast<uint8_t*>(const_cast<void*>(raw())) + offset;
    slice->size_ = length;
    slice->address() = address() + offset;
    slice->ref_counter() = ref_counter();
    return slice;
}
/** --------------------------------------------------------------------------------------------------------- SliceRec Try Claim
 * @brief Attempts to claim the SliceRec.
 * @return True if the claim was successful, false otherwise.
 */
bool SliceRec::try_claim() { return size_.try_claim(); }
/** --------------------------------------------------------------------------------------------------------- SliceRec Release
 * @brief Releases the SliceRec, resetting its state.
 */
void SliceRec::release() {
    ref_counter() = GenRefCounter();
    buffer_.reset();
    raw_ = nullptr;
    size_.release();
}
/** --------------------------------------------------------------------------------------------------------- SliceRec Destructor
 * @brief Destroys the SliceRec at teardown, when the counters beside the pool are already gone.
 */
SliceRec::~SliceRec() {}
/** --------------------------------------------------------------------------------------------------------- SliceRec Size
 * @brief Returns the size of the SliceRec.
 * @return The size of the SliceRec.
 */
size_t SliceRec::size() const { return size_.size(); }
/** --------------------------------------------------------------------------------------------------------- SliceRec Set Size
 * @brief Sets the size of the SliceRec.
 * @param size The new size.
 */
void SliceRec::set_size(size_t size) { size_ = size; }
/** --------------------------------------------------------------------------------------------------------- SliceRec Capacity
 * @brief Returns the capacity of the SliceRec.
 * @return The capacity of the SliceRec.
 */
size_t SliceRec::capacity() const { return size_.capacity(); }
/** --------------------------------------------------------------------------------------------------------- SliceRec Buffer
 * @brief Returns the buffer associated with the SliceRec.
 * @return The shared pointer to the buffer.
 */
std::shared_ptr<IBuffet> SliceRec::buffer() const { return buffer_; }
/** --------------------------------------------------------------------------------------------------------- SliceRec Set Buffer
 * @brief Sets the buffer associated with the SliceRec.
 * @param buffer The new buffer.
 */
void SliceRec::set_buffer(std::shared_ptr<IBuffet> buffer) { buffer_ = std::move(buffer); }
/** --------------------------------------------------------------------------------------------------------- SliceRec Raw
 * @brief Returns the raw pointer for the SliceRec.
 * @return The raw pointer.
 */
void* SliceRec::raw() { return raw_; }
/** --------------------------------------------------------------------------------------------------------- SliceRec Raw (const)
 * @brief Returns the raw pointer for the SliceRec.
 * @return The raw pointer.
 */
const void* SliceRec::raw() const { return raw_; }
/** --------------------------------------------------------------------------------------------------------- SliceRec Raw
 * @brief Returns the raw pointer for the SliceRec.
 * @return The raw pointer.
 */
void SliceRec::set_raw(void* raw) { raw_ = raw; }
/** --------------------------------------------------------------------------------------------------------- SliceRec ID
 * @brief Returns the ID of the current SliceRec within the pool.
 * @return The ID of the SliceRec.
 */
size_t SliceRec::id() const { return this - static_cast<SliceRec*>(IBuffet::pool()); }
/** --------------------------------------------------------------------------------------------------------- SliceRec Set
 * @brief Sets the buffer, raw pointer, and size for the SliceRec.
 * @param buffer The new buffer.
 * @param raw The new raw pointer.
 * @param size The new size.
 */
void SliceRec::set(std::shared_ptr<IBuffet> buffer, void* raw, size_t size) {
    buffer_ = std::move(buffer);
    raw_ = raw;
    size_ = size;
}
/** --------------------------------------------------------------------------------------------------------- SliceRec Set (other)
 * @brief Sets the SliceRec to match another SliceRec.
 * @param other The other SliceRec to copy from.
 */
void SliceRec::set(const SliceRec& other) {
    buffer_ = other.buffer_;
    raw_ = other.raw_;
    size_ = other.size();
    ref_counter() = other.ref_counter();
}
/** --------------------------------------------------------------------------------------------------------- SliceRec Address
 * @brief Returns the address of the SliceRec.
 * @return The address.
 */
uint64_t& SliceRec::address() { return IBuffet::addresses()[id()]; }
/** --------------------------------------------------------------------------------------------------------- SliceRec Address (const)
 * @brief Returns the address of the SliceRec.
 * @return The address.
 */
const uint64_t& SliceRec::address() const { return IBuffet::addresses()[id()]; }
/** --------------------------------------------------------------------------------------------------------- SliceRec Ref Counter
 * @brief Returns the reference counter of the SliceRec.
 * @return The reference counter.
 */
GenRefCounter& SliceRec::ref_counter() { return IBuffet::ref_counters()[id()]; }
/** --------------------------------------------------------------------------------------------------------- SliceRec Ref Counter (const)
 * @brief Returns the reference counter of the SliceRec.
 * @return The reference counter.
 */
const GenRefCounter& SliceRec::ref_counter() const { return IBuffet::ref_counters()[id()]; }
} // namespace buffetalligator