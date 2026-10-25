/** --------------------------------------------------------------------------------------------------------- Global Slice Startup Test
 * @file global_slice_startup_test.cpp
 * @brief Verifies CPU-first global Slice lifetime without including the executor header.
 */
#include <alligator.hpp>
#include <alligator/atomics.hpp>
#include <alligator/easygpu.hpp>
#include <atomic>
#include <cstdlib>
#include <utility>

using namespace buffetalligator;
namespace {
constinit std::atomic<size_t> allocations{0};
constinit std::atomic<size_t> releases{0};
/** --------------------------------------------------------------------------------------------------------- Counted Heap
 * @brief A heap placement that counts the buffers production Slice reservations create and release.
 */
struct alignas(16) CountedHeap {
    AlignedHeapBuffer memory;
    CountedHeap(size_t bytes = 0) : memory(bytes) {
        if (bytes != 0) allocations.fetch_add(1, std::memory_order_relaxed);
    }
    CountedHeap(const CountedHeap&) = delete;
    CountedHeap& operator=(const CountedHeap&) = delete;
    CountedHeap(CountedHeap&& other) noexcept : memory(std::move(other.memory)) {}
    CountedHeap& operator=(CountedHeap&& other) noexcept { memory = std::move(other.memory); return *this; }
    ~CountedHeap() { if (memory) releases.fetch_add(1, std::memory_order_relaxed); }
    operator bool() const { return bool(memory); }
    bool operator==(std::nullptr_t) const { return memory == nullptr; }
    static void deleter_impl(void* buffer) { delete static_cast<CountedHeap*>(buffer); }
    static void (*deleter())(void*) { return deleter_impl; }
    static void* host_ptr(void* buffer) { return static_cast<CountedHeap*>(buffer)->memory.raw(); }
    static size_t size_of(void* buffer) { return static_cast<CountedHeap*>(buffer)->memory.size(); }
    static void* factory(size_t bytes) { return new CountedHeap(bytes); }
    static size_t default_size() { return 256; }
    static size_t* type_idx() { static size_t idx = 0; return &idx; }
    static const char* type_name() { return "GlobalStartup"; }
    static uint64_t device_address(void* buffer) { return reinterpret_cast<uint64_t>(host_ptr(buffer)); }
    size_t size() const { return memory.size(); }
    void* raw() { return memory.raw(); }
    const void* raw() const { return memory.raw(); }
};
static_assert(IsABuffetType<CountedHeap>, "CountedHeap must satisfy IsABuffetType");
/** --------------------------------------------------------------------------------------------------------- Exit Check
 * @brief Checks final reclamation after the global owner retires, without using an already-destroyed logger.
 */
struct ExitCheck {
    ~ExitCheck() {
        if (allocations.load(std::memory_order_relaxed) == 0
            || allocations.load(std::memory_order_relaxed) != releases.load(std::memory_order_relaxed))
            std::_Exit(EXIT_FAILURE);
    }
};
ExitCheck exit_check;
/** --------------------------------------------------------------------------------------------------------- Global Owner
 * @brief Constructs arena-backed state before main and releases it before arena shutdown.
 */
struct GlobalOwner {
    Slice dedicated;
    Slice view;
    Slice chained;
    GlobalOwner() {
        const BuffetDescriptor* placement = BuffetDescriptors::descriptor<CountedHeap>();
        dedicated = Slice(193, true, placement);
        dedicated.data<uint64_t>()[8] = 0x123456789abcdef0ull;
        view = dedicated.slice(65, 65);
        chained = Slice(64, placement);
        chained.get_as<uint32_t>() = 137;
        Slice registered(64, true, placement);
        registered.get_as<uint32_t>() = 419;
        AtomicRegistry::create_global("startup_global_slice", std::move(registered));
    }
    ~GlobalOwner() {
        if (view.get_as<uint64_t>() != 0x123456789abcdef0ull
            || chained.get_as<uint32_t>() != 137) std::abort();
        dedicated.free();
        view.free();
        chained.free();
    }
};
GlobalOwner owner;
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Confirms global CPU construction completed before process-exit ownership checks run.
 */
int main() {
    if (GPU::exists() || owner.dedicated.size_bytes() != 256 || owner.view.size_bytes() != 128)
        return EXIT_FAILURE;
    LOG_INFO_STREAM << "CPU global Slice initialization passed; verifying final release at process exit";
    return 0;
}
