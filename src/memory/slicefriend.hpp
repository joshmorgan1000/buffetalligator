#pragma once
/** --------------------------------------------------------------------------------------------------------- SliceFriend
 * @file slicefriend.hpp
 * @brief Helpers for the Alligator.
 */
#include <logging.hpp>
#include <buffetalligator.hpp>
#include <memory/buffet.hpp>
#include <array>
#include <atomic>
#include <exception>
#include <thread>
#include <future>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- SlotSpan
 * @struct SlotSpan
 * @brief Represents a combined slot and span identifier packed into a 32-bit integer.
 */
struct SlotSpan {
    /// @brief The slot identifier within the arena.
    uint32_t slot_id : 17 = 0;
    /// @brief The span identifier within the slot.
    uint32_t span_id : 15 = 0;
    SlotSpan(const uint32_t& input) {
        *this = *reinterpret_cast<const SlotSpan*>(&input);
    }
    operator uint32_t() const {
        return *reinterpret_cast<const uint32_t*>(this);
    }
};
/** --------------------------------------------------------------------------------------------------------- Span64
 * @struct Span64
 * @brief Represents a span within the arena, encoded as a 64-bit integer.
 */
struct Span64 {
private:
    /// @brief The offset of the span within the arena, in 4KB units.
    uint64_t offset_   : 28 = 0;
    /// @brief The size of the span, in 64B units.
    uint64_t size_     : 36 = 0;
public:
    /** ------------------------------------------------------------------------------------------- Null Check
     * @brief Checks if the span is null (i.e., encoded value is zero).
     */
    bool is_null() const { return *reinterpret_cast<const uint64_t*>(this) == 0; }
    /** ------------------------------------------------------------------------------------------- Boolean Conversion
     * @brief Converts the span to a boolean, indicating whether it is non-null.
     */
    operator bool() const { return !is_null(); }
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs a Span64 from a 64-bit encoded value.
     * @param input The 64-bit encoded value representing the span.
     */
    Span64(const uint64_t& input) {
        *this = *reinterpret_cast<const Span64*>(&input);
    }
    /** ------------------------------------------------------------------------------------------- Conversion Operator
     * @brief Converts the Span64 to its 64-bit encoded representation.
     */
    operator uint64_t() const {
        return *reinterpret_cast<const uint64_t*>(this);
    }
    /** ------------------------------------------------------------------------------------------- Size Getter
     * @brief Returns the size of the span in bytes.
     */
    size_t size() const { return static_cast<size_t>((size_ - 1) << 6); }
    /** ------------------------------------------------------------------------------------------- Offset Getter
     * @brief Returns the offset of the span in bytes.
     */
    size_t offset() const { return static_cast<size_t>((offset_ - 1) << 12); }
    /** ------------------------------------------------------------------------------------------- Size Setter
     * @brief Sets the size of the span in bytes.
     */
    void set_size(size_t size) { size_ = ((size + 63) >> 6) + 1; }
    /** ------------------------------------------------------------------------------------------- Offset Setter
     * @brief Sets the offset of the span in bytes.
     */
    void set_offset(size_t offset) { offset_ = ((offset + 4095) >> 12) + 1; }
};
/** --------------------------------------------------------------------------------------------------------- SliceFriend
 * @class SliceFriend
 * @brief Provides access to the private encoded_ member of the Slice class.
 */
class SliceFriend {
private:
    uint32_t* encoded_;
public:
    SliceFriend(const Slice& slice)
    : encoded_(const_cast<uint32_t*>(&slice.encoded_)) {}
    SlotSpan& enc() { return *reinterpret_cast<SlotSpan*>(encoded_); }
    const SlotSpan& enc() const { return *reinterpret_cast<const SlotSpan*>(encoded_); }
    uint32_t slot_id() const { return enc().slot_id; }
    void set_slot_id(uint32_t slot_id) { enc().slot_id = slot_id; }
    uint32_t span_id() const { return enc().span_id; }
    void set_span_id(uint32_t span_id) { enc().span_id = span_id; }
    void* raw();
    const void* raw() const;
    std::atomic<int32_t>* ref_count();
    const std::atomic<int32_t>* ref_count() const;
    Buffet* buffer();
    const Buffet* buffer() const;
};
/** --------------------------------------------------------------------------------------------------------- BuffetOrder
 * @struct BuffetOrder
 * @brief Represents an order for the buffet with a promise, task, and context.
 */
struct BuffetOrder {
    std::promise<void*>* promise;
    void* (*task)(void*);
    void* context;
    void (*deleter)(BuffetOrder*);
    BuffetOrder(
        void* context_,
        void* (*task_)(void*),
        void (*deleter_)(BuffetOrder*)
    ) : promise(nullptr)
    , task(task_)
    , context(context_)
    , deleter(deleter_) {}
    ~BuffetOrder() {
        if (deleter) {
            deleter(this);
        }
    }
    BuffetOrder(const BuffetOrder&) = delete;
    BuffetOrder& operator=(const BuffetOrder&) = delete;
    BuffetOrder(BuffetOrder&&) = delete;
    BuffetOrder& operator=(BuffetOrder&&) = delete;
    std::future<void*> get_future() {
        return promise ? promise->get_future() : std::future<void*>();
    }
    static std::pair<BuffetOrder*, std::future<void*>> create(
        void* context,
        void* (*task)(void*),
        void (*deleter)(BuffetOrder*)
    ) {
        auto order = new BuffetOrder(context, task, deleter);
        order->promise = new std::promise<void*>();
        return {order, order->promise->get_future()};
    }
};
} // namespace buffetalligator