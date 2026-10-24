/** --------------------------------------------------------------------------------------------------------- SharedBuffet
 * @file sharedbuffet.cpp
 * @brief The shared handle every slot on a link points at, and the spread reference count behind it.
 */
#include <alligator.hpp>
#include <atomic>
#include <cstdint>
#include <thread>
#include <cstring>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Counter
 * @brief Returns a reference to the global atomic counter used for assigning thread IDs.
 * @return A reference to the global atomic counter.
 */
std::atomic<size_t>& MultiAtomic::counter() {
    static std::atomic<size_t> c{0};
    return c;
}
/** --------------------------------------------------------------------------------------------------------- Thread ID
 * @brief Returns the thread-local ID for the current thread.
 * @return The thread-local ID.
 */
size_t MultiAtomic::thread_id() {
    static thread_local size_t id = counter().fetch_add(1, std::memory_order_relaxed);
    return id & std::max<size_t>(num_atomics() - 1, 1);
}
/** --------------------------------------------------------------------------------------------------------- Number of Atomics
 * @brief Returns the number of atomic counters to use, based on hardware concurrency.
 * @return The total number of atomic counters to use, which is the number of hardware threads divided by 2,
 */
size_t MultiAtomic::num_atomics() {
    static size_t num = std::max<size_t>(std::thread::hardware_concurrency() >> 1, 1);
    return num;
}
/** --------------------------------------------------------------------------------------------------------- Create Atomic Group
 * @brief Sets up the initial list of integers for atomic reference counting. We don't use atomics for the
 * initial setup since it is not required - no other cache lines will have the values yet so they are all
 * forced to load on their first access.
 */
MultiAtomic::MultiAtomic() {
    int* atomics = new int[num_atomics()];
    std::memset(atomics, 0, num_atomics() * sizeof(int));
    atomics[thread_id()] = 1;
    refs_ = atomics;
}
/** --------------------------------------------------------------------------------------------------------- Find and Decrement Nonzero Atomic
 * @brief Decrements the current thread's atomic counter, even if it causes the counter to go negative, and
 * then returns the sum of all the atomic counters. We do things this way because atomic loads are over 100x
 * cheaper than atomic fetch-and-subtract operations when under contention.
 * @return The sum of all atomic counters after decrementing the current thread's counter.
 */
int MultiAtomic::decrement_refcount() {
    while (true) {
        /// The mark drives this stripe far negative, so another thread's sweep sees a decrement in flight.
        std::atomic_ref<int>(refs_[thread_id()]).fetch_sub(20000000, std::memory_order_acq_rel);
        int sum = 0;
        for (size_t i = 1; i < num_atomics(); ++i) {
            sum += std::atomic_ref<int>(refs_[(i + thread_id()) & (num_atomics() - 1)]).load(std::memory_order_acquire);
        }
        if (sum < -10000000) {
            std::atomic_ref<int>(refs_[thread_id()]).fetch_add(20000000, std::memory_order_acq_rel);
            int random_wait = std::rand() % 50;
            for (int i = 0; i < random_wait; ++i) std::this_thread::yield();
            continue;
        }
        /// Commit: lift the mark and take the one, and count this stripe in the total.
        const int own = std::atomic_ref<int>(refs_[thread_id()]).fetch_add(19999999, std::memory_order_acq_rel) + 19999999;
        return sum + own;
    }
}
/** --------------------------------------------------------------------------------------------------------- Ref Count Accessor
 * @brief Retrieves the total reference count by summing all atomic counters.
 * @return The total reference count.
 */
int MultiAtomic::refcount() const {
    int sum = 0;
    for (size_t i = 0; i < num_atomics(); ++i) {
        sum += std::atomic_ref<int>(refs_[i]).load(std::memory_order_acquire);
    }
    return sum;
}
/** --------------------------------------------------------------------------------------------------------- Increment Atomic
 * @brief Increments the atomic counter for the current thread.
 */
void MultiAtomic::increment_refcount() {
    std::atomic_ref<int>(refs_[thread_id()]).fetch_add(1, std::memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- SharedBuffet Constructor
 * @brief Makes a dedicated link under this handle; the count starts at one for the slot that takes it.
 * @param descriptor The buffet descriptor.
 * @param size The size of the buffer to allocate.
 * @param novel_buffer True for a dedicated buffer, which never keeps a link ahead.
 */
SharedBuffet::SharedBuffet(const BuffetDescriptor* descriptor, size_t size, bool novel_buffer) {
    data_ = new Data{nullptr, MultiAtomic()};
    data_->chainbuffet = new Alligator::ChainBuffet(this, descriptor, size, !novel_buffer);
}
/** --------------------------------------------------------------------------------------------------------- SharedBuffet Destructor
 * @brief Deleting the handle once its count has reached zero frees the link and the shared count; a handle
 * still counted leaves them for the holders that remain.
 */
SharedBuffet::~SharedBuffet() {
    if (!data_ || data_->refs.refcount() != 0) return;
    auto* link = static_cast<Alligator::ChainBuffet*>(std::exchange(data_->chainbuffet, nullptr));
    if (link && link->parent_ == this) link->parent_ = nullptr;
    delete link;
    delete data_;
    data_ = nullptr;
}
/** --------------------------------------------------------------------------------------------------------- SharedBuffet Raw Pointer Accessor
 * @brief Retrieves the raw host pointer of the link's buffer.
 * @return A raw pointer to the host memory, null once the link is gone.
 */
void* SharedBuffet::raw() {
    auto* link = static_cast<Alligator::ChainBuffet*>(data_->chainbuffet);
    return link ? link->desc->host_ptr(link->buffer_) : nullptr;
}
const void* SharedBuffet::raw() const {
    auto* link = static_cast<Alligator::ChainBuffet*>(data_->chainbuffet);
    return link ? link->desc->host_ptr(link->buffer_) : nullptr;
}
/** --------------------------------------------------------------------------------------------------------- SharedBuffet Size Accessor
 * @brief Retrieves the byte size of the link's buffer.
 * @return The size in bytes, zero once the link is gone.
 */
size_t SharedBuffet::size() const {
    auto* link = static_cast<Alligator::ChainBuffet*>(data_->chainbuffet);
    return link ? link->desc->size_of(link->buffer_) : 0;
}
/** --------------------------------------------------------------------------------------------------------- SharedBuffet Descriptor Accessor
 * @brief The placement the link's buffer came from.
 * @return The descriptor, null once the link is gone.
 */
const BuffetDescriptor* SharedBuffet::descriptor() const {
    auto* link = static_cast<Alligator::ChainBuffet*>(data_->chainbuffet);
    return link ? link->desc : nullptr;
}
} // namespace buffetalligator
