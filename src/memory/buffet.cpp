/** --------------------------------------------------------------------------------------------------------- Buffer Implementation
 * @file buffet.cpp
 * @brief Implementation of the base generic buffer types.
 */
#include <alligator.hpp>
#include <gpu/easyvulkan.hpp>
#include <gpu/backend.hpp>
#include <cstring>
#include <cstdint>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- IBuffet Constructor
 * @brief Constructs an IBuffet with the given size.
 * @param size The size of the buffer.
 */
IBuffet::IBuffet(size_t size) : size_(size) {}
/** --------------------------------------------------------------------------------------------------------- IBuffet Destructor
 * @brief Destroys the IBuffet.
 */
IBuffet::~IBuffet() { free(); }
/** --------------------------------------------------------------------------------------------------------- Thread ID
 * @brief Retrieves the current thread ID.
 * @return The current thread ID.
 */
size_t IBuffet::threadid() {
    static thread_local size_t thid =
        allocator().tid.fetch_add(1, std::memory_order_relaxed)
            % allocator().current.size();
    return thid;
}
/** --------------------------------------------------------------------------------------------------------- Mapped
 * @brief Retrieves the mapped pointer of the buffer.
 * @return The mapped pointer.
 */
void* IBuffet::mapped() const { return const_cast<void*>(host_ptr()); }
/** --------------------------------------------------------------------------------------------------------- Address
 * @brief Retrieves the device address of the buffer.
 * @return The device address.
 */
uint64_t IBuffet::address() const { return device_address(); }
/** --------------------------------------------------------------------------------------------------------- Claim
 * @brief Claims a slice of the buffer.
 * @param size The size of the slice.
 * @param novel Whether to allocate a novel buffer.
 * @return The claimed slice.
 */
Slice IBuffet::claim(uint32_t size, bool novel) {
    std::shared_ptr<IBuffet> current = IBuffet::current();
    if (size == 0) return Slice{};
    Allocator& alloc = allocator();
    if (novel || size > 64u * 1024u * 1024u) {
        std::shared_ptr<IBuffet> dedicated = default_factory()();
        dedicated->set_size(size);
        dedicated->allocate();
        SliceRec* rec = IBuffet::next_available();
        rec->set_size(size);
        rec->buffer_ = std::move(dedicated);
        rec->raw_ = rec->buffer_->host_ptr();
        alloc.addresses[rec->id()] = rec->buffer_->address();
        return Slice(rec->buffer_->address(), rec->raw_, rec->id(), size);
    }
    const uint32_t units = (size + 63u) >> 6;
    const uint32_t start = current->bump_.fetch_add(units, std::memory_order_relaxed);
    if (start + units > (current->size_.size() >> 6)) {
        std::shared_ptr<IBuffet> next;
        if (start <= (current->size_.size() >> 6)) {
            if (!alloc.fresh.try_dequeue(next)) {
                std::shared_ptr<IBuffet> desired = default_factory()();
                desired->allocate();
                std::atomic_store_explicit(
                    &alloc.current[threadid()],
                    desired,
                    std::memory_order_release
                );
                std::shared_ptr<IBuffet> extra = default_factory()();
                extra->allocate();
                alloc.fresh.enqueue(extra);
            } else {
                // Successfully dequeued a fresh buffer, time to install it!
                std::atomic_store_explicit(
                    &alloc.current[threadid()],
                    next,
                    std::memory_order_release
                );
                if (alloc.fresh.size_approx() < 1) {
                    std::shared_ptr<IBuffet> extra = default_factory()();
                    extra->allocate();
                    alloc.fresh.enqueue(extra);
                }
                return next->claim(size);
            }
        }
        std::shared_ptr<IBuffet> other = std::atomic_load_explicit(
            &alloc.current[threadid()], std::memory_order_acquire
        );
        while (current == other) {
            std::this_thread::yield();
            other = std::atomic_load_explicit(
                &alloc.current[threadid()], std::memory_order_acquire
            );
        }
        return other->claim(size);
    }
    const uint32_t offset = start << 6;
    const uint32_t idx = alloc.next_available(current, offset, size);
    alloc.pool[idx].raw_ = static_cast<char*>(current->host_ptr()) + offset;
    alloc.addresses[idx] = current->device_address() + offset;
    return Slice(alloc.addresses[idx], alloc.pool[idx].raw_, idx, size);
}
/** --------------------------------------------------------------------------------------------------------- Next Available
 * @brief Retrieves the next available slice record.
 * @return Pointer to the next available slice record.
 */
SliceRec* IBuffet::next_available() { return allocator().next_available(); }
/** --------------------------------------------------------------------------------------------------------- Ref
 * @brief Retrieves a reference to a slice record by index.
 * @param idx The index of the slice record.
 * @return Pointer to the slice record.
 */
SliceRec* IBuffet::ref(uint32_t idx) { return &allocator().pool[idx & 0xFFFFFu]; }
/** --------------------------------------------------------------------------------------------------------- Allocate
 * @brief Allocates the buffer.
 * Default implementation does nothing.
 */
void IBuffet::allocate() {
    // Default implementation does nothing.
}
/** --------------------------------------------------------------------------------------------------------- Device Address
 * @brief Retrieves the device address of the buffer.
 * @return The device address.
 */
uint64_t IBuffet::device_address() const { return 0; }
/** --------------------------------------------------------------------------------------------------------- Pool
 * @brief Retrieves the pool of slice records.
 * @return Pointer to the pool.
 */
void* IBuffet::pool() { return &allocator().pool; }
/** --------------------------------------------------------------------------------------------------------- Device Table
 * @brief Retrieves the device address table.
 * @return The device address table.
 */
uint64_t IBuffet::device_table() { return allocator().address_table; }
/** --------------------------------------------------------------------------------------------------------- Addresses
 * @brief Retrieves the array of addresses for the buffer slices.
 * @return Pointer to the array of addresses.
 */
uint64_t* IBuffet::addresses() { return allocator().addresses; }
/** --------------------------------------------------------------------------------------------------------- Addr
 * @brief Retrieves the address of a specific buffer slice by index.
 * @param index The index of the buffer slice.
 * @return Reference to the address of the buffer slice.
 */
uint64_t& IBuffet::addr(uint32_t index) { return allocator().addresses[index]; }
/** --------------------------------------------------------------------------------------------------------- Default Factory
 * @brief Retrieves the default buffer factory function.
 * @return Reference to the default buffer factory function.
 */
IBuffet::DefaultBufferFactoryFn& IBuffet::default_factory() {
    static DefaultBufferFactoryFn factory = nullptr;
    return factory;
}
/** --------------------------------------------------------------------------------------------------------- Set Default Factory
 * @brief Registers the factory every chain buffer comes from and builds the address table with it.
 * @param factory The factory, a static method of the backend's buffer type.
 */
void IBuffet::set_default_factory(DefaultBufferFactoryFn factory) {
    Allocator& alloc = allocator();
    default_factory() = factory;
    alloc.table = factory();
    alloc.table->set_size(sizeof(uint64_t) * 0x100000);
    alloc.table->allocate();
    alloc.addresses = static_cast<uint64_t*>(alloc.table->host_ptr());
    alloc.address_table = alloc.table->device_address();
}
/** --------------------------------------------------------------------------------------------------------- Current
 * @brief Retrieves the current buffer for the thread.
 * @return Shared pointer to the current buffer.
 */
std::shared_ptr<IBuffet> IBuffet::current() {
    std::shared_ptr<IBuffet> current = std::atomic_load_explicit(
        &allocator().current[threadid()], std::memory_order_acquire
    );
    if (current == nullptr) {
        ensure_table();
        std::shared_ptr<IBuffet> v;
        if (!allocator().fresh.try_dequeue(v)) {
            std::shared_ptr<IBuffet> fresh = default_factory()();
            fresh->allocate();
            std::atomic_store_explicit(
                &allocator().current[threadid()],
                std::move(fresh),
                std::memory_order_release
            );
            fresh = default_factory()();
            fresh->allocate();
            allocator().fresh.enqueue(fresh);
        } else {
            std::atomic_store_explicit(
                &allocator().current[threadid()],
                std::move(v),
                std::memory_order_release
            );
            if (allocator().fresh.size_approx() < 1) {
                std::shared_ptr<IBuffet> fresh = default_factory()();
                fresh->allocate();
                allocator().fresh.enqueue(fresh);
            }
        }
    }
    return allocator().current[threadid()];
}
/** --------------------------------------------------------------------------------------------------------- Ensure Table
 * @brief Ensures that the address table is built.
 */
void IBuffet::ensure_table() {
    static std::once_flag built;
    std::call_once(built, &IBuffet::build_table);
}
/** --------------------------------------------------------------------------------------------------------- Build Table
 * @brief Freezes the backend selection, whose Vulkan setup registers the default factory and builds the table.
 */
void IBuffet::build_table() { static_cast<void>(GPUStatic::gpu_selection()); }
/** --------------------------------------------------------------------------------------------------------- Recycle
 * @brief Cleans a spent chain buffer back into fresh when fresh is empty, and deletes it otherwise.
 * @param buffer Pointer to the buffer to recycle.
 */
void IBuffet::recycle(IBuffet* buffer) {
    Allocator& alloc = allocator();
    if (!alloc.stopping && alloc.fresh.size_approx() == 0 && buffer->size() == 64u * 1024u * 1024u) {
        std::memset(buffer->host_ptr(), 0, buffer->size());
        buffer->bump_.store(0, std::memory_order_relaxed);
        alloc.fresh.enqueue(std::shared_ptr<IBuffet>(buffer, &recycle));
    } else delete buffer;
}
/** --------------------------------------------------------------------------------------------------------- Shutdown
 * @brief Shuts down the allocator, releasing all buffers.
 */
void IBuffet::shutdown() {
    Allocator& alloc = allocator();
    alloc.stopping = true;
    for (std::shared_ptr<IBuffet>& buffer : alloc.current) buffer.reset();
    std::shared_ptr<IBuffet> spare;
    while (alloc.fresh.try_dequeue(spare)) spare.reset();
}
/** --------------------------------------------------------------------------------------------------------- Copy Record
 * @brief Copies a slice record and associates it with a device address.
 * @param rec The slice record to copy.
 * @param address The device address to associate with the slice.
 * @return The index of the copied slice in the allocator's pool.
 */
uint32_t IBuffet::copy_rec(const SliceRec& rec, vk::DeviceAddress address) {
    Allocator& alloc = allocator();
    SliceRec* fresh = alloc.next_available();
    fresh->set(rec);
    const uint32_t idx = static_cast<uint32_t>(fresh - alloc.pool.data());
    alloc.addresses[idx] = address;
    return idx;
}
/** --------------------------------------------------------------------------------------------------------- Embue ID
 * @brief Embues a buffer with an ID and returns the corresponding slice.
 * @param buffer The buffer to embue.
 * @return The slice representing the embued buffer.
 */
Slice IBuffet::embue_id(std::shared_ptr<IBuffet> buffer) {
    ensure_table();
    Allocator& alloc = allocator();
    const uint32_t idx = alloc.next_available(buffer, 0, buffer->size());
    alloc.pool[idx].raw_ = buffer->host_ptr();
    alloc.addresses[idx] = buffer->device_address();
    return Slice(alloc.addresses[idx], alloc.pool[idx].raw_, idx, buffer->size());
}
/** --------------------------------------------------------------------------------------------------------- AlignedHeapBuffer
 * @brief Implementation of the AlignedHeapBuffer class, providing aligned memory allocation and management.
 */
AlignedHeapBuffer::~AlignedHeapBuffer() { free(); }
/** --------------------------------------------------------------------------------------------------------- Allocate
 * @brief Allocates aligned memory for the buffer if it hasn't been allocated yet.
 */
void AlignedHeapBuffer::allocate() {
    if (host_ptr_) return;
    host_ptr_ = std::aligned_alloc(64, size_.capacity());
    std::memset(host_ptr_, 0, size_);
}
/** --------------------------------------------------------------------------------------------------------- Free
 * @brief Frees the allocated memory for the buffer if it has been allocated.
 */
void AlignedHeapBuffer::free() {
    if (host_ptr_) { std::free(host_ptr_); host_ptr_ = nullptr; }
}
/** --------------------------------------------------------------------------------------------------------- Size
 * @brief Retrieves the size of the buffer.
 * @return The size of the buffer in bytes.
 */
size_t AlignedHeapBuffer::size() const { return size_; }
/** --------------------------------------------------------------------------------------------------------- Set Size
 * @brief Sets the size of the buffer.
 * @param size The new size of the buffer in bytes.
 */
void AlignedHeapBuffer::set_size(size_t size) { size_ = size; }
/** --------------------------------------------------------------------------------------------------------- Host Pointer
 * @brief Retrieves the host pointer to the buffer's memory.
 * @return Pointer to the buffer's memory.
 */
void* AlignedHeapBuffer::host_ptr() { return host_ptr_; }
/** --------------------------------------------------------------------------------------------------------- Host Pointer (Const)
 * @brief Retrieves the host pointer to the buffer's memory (const version).
 * @return Const pointer to the buffer's memory.
 */
const void* AlignedHeapBuffer::host_ptr() const { return host_ptr_; }
} // namespace buffetalligator