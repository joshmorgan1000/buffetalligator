/** --------------------------------------------------------------------------------------------------------- Future Slice Test
 * @file future_slice_test.cpp
 * @brief Checks arena-backed future ownership, completion ordering, and callback failures.
 */
#include <alligator/kitchen.hpp>
#include "functional_support.hpp"
#include <cstdint>
#include <thread>
#include <utility>

namespace {
using buffetalligator::FutureSlice;
using buffetalligator::KitchenException;
using buffetalligator::Slice;
using buffetalligator::SlicePromise;
using functional::require;
static_assert(sizeof(FutureSlice) == sizeof(Slice));
/** --------------------------------------------------------------------------------------------------------- Payload
 * @brief Creates a host-accessible result with a known value.
 */
Slice payload() {
    Slice slice(size_t{64}, buffetalligator::BuffetDescriptors::descriptor_for(
        static_cast<buffetalligator::AlignedHeapBuffer*>(nullptr)));
    slice.get_as<uint64_t>() = 41;
    return slice;
}
/** --------------------------------------------------------------------------------------------------------- Complete
 * @brief Completes a promise after transferring it to another thread.
 */
void complete(SlicePromise promise, Slice slice) {
    promise.set(std::move(slice));
}
/** --------------------------------------------------------------------------------------------------------- Callback
 * @brief Updates the result before its completion signal is published.
 */
void callback(Slice slice) {
    ++slice.get_as<uint64_t>();
}
/** --------------------------------------------------------------------------------------------------------- Failing Callback
 * @brief Throws an identifiable error from the completion callback.
 */
void failing_callback(Slice) {
    ALLIGATOR_KITCHEN_THROW("future callback failure");
}
/** --------------------------------------------------------------------------------------------------------- Move Handles
 * @brief Preserves a pending result through promise and future move construction and assignment.
 */
void move_handles() {
    Slice slice = payload();
    SlicePromise source;
    FutureSlice original = source.get_future();
    SlicePromise moved(std::move(source));
    SlicePromise assigned;
    assigned = std::move(moved);
    SlicePromise replaced;
    FutureSlice target = replaced.get_future();
    target = std::move(original);
    FutureSlice future(std::move(target));
    std::thread producer(&complete, std::move(assigned), slice);
    Slice result = future.get();
    require(result.raw() == slice.raw(), "moving handles copied the result");
    require(result.get_as<uint64_t>() == 41, "moving handles lost the result");
    producer.join();
}
/** --------------------------------------------------------------------------------------------------------- Complete Before Retrieval
 * @brief Retains a completion signal until its future is retrieved and consumed.
 */
void complete_before_retrieval() {
    SlicePromise promise;
    promise.set(payload());
    const FutureSlice future = promise.get_future();
    require(future.get().get_as<uint64_t>() == 41, "early completion lost its result");
}
/** --------------------------------------------------------------------------------------------------------- Completed Future
 * @brief Returns a completed future after destroying its producer.
 */
FutureSlice completed_future(Slice slice) {
    SlicePromise promise;
    FutureSlice future = promise.get_future();
    promise.set(std::move(slice));
    return future;
}
/** --------------------------------------------------------------------------------------------------------- Outlive Producer
 * @brief Keeps the result alive after both the producer and future are destroyed.
 */
void outlive_producer() {
    Slice result;
    {
        FutureSlice future = completed_future(payload());
        result = future.get();
    }
    require(result.get_as<uint64_t>() == 41, "completion state released the returned result");
}
/** --------------------------------------------------------------------------------------------------------- Discard Future
 * @brief Allows fire-and-forget completion after the consumer releases its future.
 */
void discard_future() {
    Slice slice = payload();
    SlicePromise promise;
    promise.set_callback(&callback);
    {
        FutureSlice discarded = promise.get_future();
    }
    promise.set(slice);
    require(slice.get_as<uint64_t>() == 42, "discarding the future prevented its callback");
}
/** --------------------------------------------------------------------------------------------------------- Callback Before Get
 * @brief Publishes callback writes before a waiting consumer receives its result.
 */
void callback_before_get() {
    SlicePromise promise;
    promise.set_callback(&callback);
    FutureSlice future = promise.get_future();
    std::thread producer(&complete, std::move(promise), payload());
    require(future.get().get_as<uint64_t>() == 42, "get returned before the callback completed");
    producer.join();
}
/** --------------------------------------------------------------------------------------------------------- Callback Failure
 * @brief Signals the future even when the callback throws and preserves the exception type.
 */
void callback_failure() {
    SlicePromise promise;
    promise.set_callback(&failing_callback);
    FutureSlice future = promise.get_future();
    bool producer_failed = false;
    try {
        promise.set(payload());
    } catch (const KitchenException&) {
        producer_failed = true;
    }
    require(producer_failed, "set swallowed the callback exception");
    bool consumer_failed = false;
    try {
        static_cast<void>(future.get());
    } catch (const KitchenException&) {
        consumer_failed = true;
    }
    require(consumer_failed, "get swallowed the callback exception");
}
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs the public promise and future completion contracts.
 */
int main() {
    LOG_INFO_STREAM << "Checking SlicePromise and FutureSlice ownership and completion";
    move_handles();
    complete_before_retrieval();
    outlive_producer();
    discard_future();
    callback_before_get();
    callback_failure();
    LOG_INFO_STREAM << "SlicePromise and FutureSlice checks passed";
    return 0;
}
