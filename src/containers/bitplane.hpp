#pragma once
/** --------------------------------------------------------------------------------------------------------- Bitplane
 * @file bitplane.hpp
 * @brief Defines the Bitplane class - a single bitplane rather than a collection of them
 */
#include <logging.hpp>
#include <alligator.hpp>
#include <chrono>
#include <atomic>
#include <cstdint>

namespace buffetalligator {
class CollectionState;
class Bitplanes;
class BitplaneChunk;
class ConcurrentBitplane;
class ConcurrentBitplaneChunk;
/** --------------------------------------------------------------------------------------------------------- Bitplane Exception
 * @class BitplaneException
 * @brief Exception class for Bitplane-related errors.
 */
class BitplaneException : public threadsafe_logger::Exception {
public:
    using threadsafe_logger::Exception::Exception;
    using threadsafe_logger::Exception::what;
};
#define ALLIGATOR_BITPLANE_THROW(msg) throw BitplaneException(msg)
/** --------------------------------------------------------------------------------------------------------- ConcurrentBitplane
 * @class ConcurrentBitplane
 * @brief Represents a single bitplane, which is a stream of bits, each bit considered a separate element.
 *
 * Example:
 * ```cpp
 * ConcurrentBitplane bitplane(1024);
 * bitplane.set(42);
 * const bool bit = bitplane.is_set(42);
 * ```
 */
class alignas(16) ConcurrentBitplane {
private:
    struct MemAlloc {
        void* mem;
        void* unaligned;
        size_t size;
        void free() {
            std::free(unaligned);
            mem = nullptr;
            unaligned = nullptr;
        }
        void resize(size_t new_size, size_t alignment) {
            size = new_size;
            unaligned = std::realloc(unaligned, size + (alignment - 1));
            mem = reinterpret_cast<void*>(
                (reinterpret_cast<uintptr_t>(unaligned) + (alignment - 1)) & ~(alignment - 1)
            );
        }
        MemAlloc(size_t init_size, size_t alignment) {
            size = init_size;
            unaligned = std::calloc(1, size + (alignment - 1));
            mem = reinterpret_cast<void*>(
                (reinterpret_cast<uintptr_t>(unaligned) + (alignment - 1)) & ~(alignment - 1)
            );
        }
        ~MemAlloc() { free(); }
        void* raw() { return unaligned; }
        const void* raw() const { return unaligned; }
        template<typename T>
        T* data() { return reinterpret_cast<T*>(mem); }
        template<typename T>
        const T* data() const { return reinterpret_cast<const T*>(mem); }
        size_t size_bytes() const { return size; }
    };
    /// @brief Shared pointer to the atomic slice representing the bitplane.
    std::shared_ptr<std::atomic<MemAlloc*>> slice_;
    /** ------------------------------------------------------------------------------------------- Atomic Word Access
     * @brief Access the atomic word at the specified index within the bitplane.
     * @param word_idx The index of the atomic word to access.
     * @return Reference to the atomic word at the specified index.
     */
    std::atomic<uint64_t>* atomic_word_at(size_t word_idx) {
        return &slice_->load(
                std::memory_order_acquire
        )->data<std::atomic<uint64_t>>()[word_idx];
    }
    /** ------------------------------------------------------------------------------------------- Atomic Word Access (Const)
     * @brief Access the atomic word at the specified index within the bitplane (const version).
     * @param word_idx The index of the atomic word to access.
     * @return Const reference to the atomic word at the specified index.
     */
    const std::atomic<uint64_t>* atomic_word_at(size_t word_idx) const {
        return &slice_->load(
                std::memory_order_acquire
        )->data<std::atomic<uint64_t>>()[word_idx];
    }
public:
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs a Bitplane object with the specified parameters.
     * @param size_in_bits The size of the bitplane in bits.
     * @param novel_slice Indicates whether this bitplane is part of a novel slice.
     */
    ConcurrentBitplane(
        size_t size_in_bits = 0
    ) : slice_(std::make_shared<std::atomic<MemAlloc*>>(new MemAlloc((size_in_bits + 7) >> 3, 16))) {}
    /** ------------------------------------------------------------------------------------------- Copy/Move Constructors and Assignment Operators */
    ConcurrentBitplane(const ConcurrentBitplane& other) : slice_(other.slice_) {}
    ConcurrentBitplane& operator=(const ConcurrentBitplane& other) {
        slice_ = other.slice_; return *this; }
    ConcurrentBitplane(ConcurrentBitplane&& other) noexcept : slice_(std::move(other.slice_)) {}
    ConcurrentBitplane& operator=(ConcurrentBitplane&& other) noexcept {
        slice_ = std::move(other.slice_);
        return *this;
    }
    /** ------------------------------------------------------------------------------------------- Destructor */
    ~ConcurrentBitplane() = default;
    /** ------------------------------------------------------------------------------------------- size
     * @brief Get the number of bits in the bitplane.
     * @return The number of bits in the bitplane.
     */
    size_t size() const {
        return size_bytes() << 3;
    }
    /** ------------------------------------------------------------------------------------------- Size in Bytes
     * @brief Get the size of the bitplane in bytes, aligned to 256 bytes for SIMD operations.
     * @return The size of the bitplane in bytes, aligned to 256 bytes.
     */
    size_t size_bytes() const { return slice_->load(std::memory_order_acquire)->size_bytes(); }
    /** ------------------------------------------------------------------------------------------- test_and_set
     * @brief Atomically test and set the bit at the specified index.
     * @param index The index of the bit to test and set.
     * @return True if the bit was already set, false otherwise.
     */
    bool test_and_set(size_t index, std::memory_order order = std::memory_order_acq_rel) {
        uint64_t mask = uint64_t(1) << (index & 63);
        return (atomic_word_at(index >> 6)->fetch_or(mask, order) & mask) != 0;
    }
    /** ------------------------------------------------------------------------------------------- test_and_clear
     * @brief Atomically test and clear the bit at the specified index.
     * @param index The index of the bit to test and clear.
     * @return True if the bit was already set, false otherwise.
     */
    bool test_and_clear(size_t index, std::memory_order order = std::memory_order_acq_rel) {
        uint64_t mask = uint64_t(1) << (index & 63);
        return (atomic_word_at(index >> 6)->fetch_and(~mask, order) & mask) != 0;
    }
    /** ------------------------------------------------------------------------------------------- is_set
     * @brief Check if the bit at the specified index is set (i.e., is 1).
     * @param index The index of the bit to check.
     * @return True if the bit at the specified index is set, false otherwise.
     */
    bool is_set(size_t index) const {
        uint64_t mask = uint64_t(1) << (index & 63);
        return (atomic_word_at(index >> 6)->load(std::memory_order_acquire) & mask) != 0;
    }
    /** ------------------------------------------------------------------------------------------- set
     * @brief Set the bit at the specified index to 1.
     * @param index The index of the bit to set.
     */
    void set(size_t index, std::memory_order order = std::memory_order_acq_rel) {
        atomic_word_at(index >> 6)->fetch_or(uint64_t(1) << (index & 63), order);
    }
    /** ------------------------------------------------------------------------------------------- clear
     * @brief Clear the bit at the specified index (i.e., set it to 0).
     * @param index The index of the bit to clear.
     */
    void clear(size_t index, std::memory_order order = std::memory_order_acq_rel) {
        atomic_word_at(index >> 6)->fetch_and(~(uint64_t(1) << (index & 63)), order);
    }
    /** ------------------------------------------------------------------------------------------- set_all
     * @brief Set every bit in the bitplane to 1.
     */
    void set_all() {
        std::memset(
            slice_->load(std::memory_order_acquire)->raw(),
            0xFF,
            slice_->load(std::memory_order_acquire)->size_bytes()
        );
    }
    /** ------------------------------------------------------------------------------------------- clear_all
     * @brief Clear every bit in the bitplane (i.e., set them all to 0).
     */
    void clear_all() {
        std::memset(
            slice_->load(std::memory_order_acquire)->raw(),
            0x00,
            slice_->load(std::memory_order_acquire)->size_bytes()
        );
    }
    /** ------------------------------------------------------------------------------------------- resize
     * @brief Resize the bitplane to the specified number of bits.
     * @param new_num_bits The new number of bits for the bitplane.
     * @param preserve_data Indicates whether to preserve the existing data.
     * @param novel_buffer Indicates whether to allocate a novel buffer for the resized bitplane.
     */
    void resize(size_t new_num_bits) {
        slice_->load(std::memory_order_acquire)->resize(
            (new_num_bits + 7) >> 3, 16
        );
    }
};
} // namespace buffetalligator
