/** --------------------------------------------------------------------------------------------------------- Slice Queue
 * @file slice_queue.cpp
 * @brief Exposes the C TLS queue through move-only Slice ownership and thread-bound handles.
 */
#include <alligator.hpp>
extern "C" {
#include "core/ba_queue.h"
}

namespace buffetalligator {
static_assert(SliceQueue::block_size == BA_QUEUE_BATCH_SIZE);
/** --------------------------------------------------------------------------------------------------------- Release Descriptor
 * @brief Releases one undelivered descriptor's arena ownership through the queue's Slice access.
 */
void SliceQueue::release_descriptor(void* descriptor) noexcept {
    ba_slice_t* carried = static_cast<ba_slice_t*>(descriptor);
    Slice slice;
    slice.meta_ = carried->meta;
    slice.cached_ = carried->ptr;
    carried->meta = UINT64_MAX;
    carried->ptr = nullptr;
    slice.free();
}
/** --------------------------------------------------------------------------------------------------------- Constructor
 * @brief Allocates the configured C queue or rejects invalid geometry.
 */
SliceQueue::SliceQueue(size_t producers, size_t consumers, size_t capacity)
: queue_(ba_queue_create(producers, consumers, capacity, &SliceQueue::release_descriptor)) {
    if (!queue_) ALLIGATOR_THROW("SliceQueue requires positive worker counts and whole-block capacity");
}
/** --------------------------------------------------------------------------------------------------------- Destructor
 * @brief Releases queue-owned messages after all worker bindings have ended.
 */
SliceQueue::~SliceQueue() { ba_queue_destroy(static_cast<ba_queue_t*>(queue_)); }
/** --------------------------------------------------------------------------------------------------------- Producer
 * @brief Claims a unique producer lane and caches the calling thread's TLS address.
 */
SliceQueue::Producer SliceQueue::producer(size_t index) {
    auto* local = ba_queue_bind_producer(static_cast<ba_queue_t*>(queue_), index);
    if (!local) ALLIGATOR_THROW("SliceQueue producer index, thread binding, or open state is invalid");
    return Producer(local);
}
/** --------------------------------------------------------------------------------------------------------- Consumer
 * @brief Claims a unique consumer mailbox and caches the calling thread's TLS address.
 */
SliceQueue::Consumer SliceQueue::consumer(size_t index) {
    auto* local = ba_queue_bind_consumer(static_cast<ba_queue_t*>(queue_), index);
    if (!local) ALLIGATOR_THROW("SliceQueue consumer index or thread binding is invalid");
    return Consumer(local);
}
/** --------------------------------------------------------------------------------------------------------- Close
 * @brief Publishes closure after the caller has synchronized with all completed producers.
 */
void SliceQueue::close() noexcept { ba_queue_close(static_cast<ba_queue_t*>(queue_)); }
/** --------------------------------------------------------------------------------------------------------- Reset
 * @brief Reopens drained storage at application quiescence.
 */
void SliceQueue::reset() noexcept { ba_queue_reset(static_cast<ba_queue_t*>(queue_)); }
/** --------------------------------------------------------------------------------------------------------- Producer Destructor
 * @brief Flushes the remaining partial block and releases the producer binding.
 */
SliceQueue::Producer::~Producer() { ba_queue_unbind(static_cast<ba_queue_local_t*>(local_)); }
/** --------------------------------------------------------------------------------------------------------- Push
 * @brief Transfers one descriptor into the C queue without retaining its backing.
 */
void SliceQueue::Producer::push(Slice&& slice) noexcept {
    ba_queue_push(static_cast<ba_queue_local_t*>(local_), reinterpret_cast<const ba_slice_t*>(&slice), 1);
    slice.meta_ = UINT64_MAX;
    slice.cached_ = nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Push Bulk
 * @brief Transfers a span of descriptors and clears the moved source handles.
 */
void SliceQueue::Producer::push(std::span<Slice> slices) noexcept {
    ba_queue_push(static_cast<ba_queue_local_t*>(local_),
        reinterpret_cast<const ba_slice_t*>(slices.data()), slices.size());
    for (auto& slice : slices) {
        slice.meta_ = UINT64_MAX;
        slice.cached_ = nullptr;
    }
}
/** --------------------------------------------------------------------------------------------------------- Flush
 * @brief Publishes the producer's current partial block.
 */
void SliceQueue::Producer::flush() noexcept { ba_queue_flush(static_cast<ba_queue_local_t*>(local_)); }
/** --------------------------------------------------------------------------------------------------------- Consumer Destructor
 * @brief Releases a binding after the consumer has drained its locally owned blocks.
 */
SliceQueue::Consumer::~Consumer() { ba_queue_unbind(static_cast<ba_queue_local_t*>(local_)); }
/** --------------------------------------------------------------------------------------------------------- Pop
 * @brief Replaces a destination only after obtaining an owned descriptor from the C queue.
 */
bool SliceQueue::Consumer::pop(Slice& output) noexcept {
    ba_slice_t received;
    if (!ba_queue_pop(static_cast<ba_queue_local_t*>(local_), &received, 1)) return false;
    output.free();
    output.meta_ = received.meta;
    output.cached_ = received.ptr;
    return true;
}
/** --------------------------------------------------------------------------------------------------------- Pop Bulk
 * @brief Replaces only the destinations filled by one block transfer.
 */
size_t SliceQueue::Consumer::pop(std::span<Slice> output) noexcept {
    if (output.empty()) return 0;
    ba_slice_t received[block_size];
    const size_t count = ba_queue_pop(static_cast<ba_queue_local_t*>(local_), received,
        std::min(output.size(), block_size));
    for (size_t index = 0; index < count; ++index) {
        output[index].free();
        output[index].meta_ = received[index].meta;
        output[index].cached_ = received[index].ptr;
    }
    return count;
}
} // namespace buffetalligator
