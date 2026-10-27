/** --------------------------------------------------------------------------------------------------------- Memory Contract Test
 * @file memory_test.cpp
 * @brief Verifies placement registration, chain rollover, Slice ownership, and tracked allocations.
 */
#include <alligator.hpp>
#include <alligator/atomics.hpp>
#include <memory/tracker.hpp>
#include "functional_support.hpp"
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <thread>
#include <utility>

using namespace buffetalligator;
using functional::require;
namespace {
std::atomic<size_t> allocations{0};
std::atomic<size_t> deallocations{0};
/** --------------------------------------------------------------------------------------------------------- Memory Placement
 * @brief Real aligned storage counted on every allocation and release.
 */
struct alignas(16) MemoryPlacement {
    AlignedHeapBuffer memory;
    MemoryPlacement(size_t bytes = 0) : memory(bytes) {
        if (bytes != 0) allocations.fetch_add(1, std::memory_order_relaxed);
    }
    MemoryPlacement(const MemoryPlacement&) = delete;
    MemoryPlacement& operator=(const MemoryPlacement&) = delete;
    MemoryPlacement(MemoryPlacement&& other) noexcept : memory(std::move(other.memory)) {}
    MemoryPlacement& operator=(MemoryPlacement&& other) noexcept {
        memory = std::move(other.memory);
        return *this;
    }
    ~MemoryPlacement() {
        if (memory) deallocations.fetch_add(1, std::memory_order_relaxed);
    }
    operator bool() const { return bool(memory); }
    bool operator==(std::nullptr_t) const { return memory == nullptr; }
    static void deleter_impl(void* buffer) { delete static_cast<MemoryPlacement*>(buffer); }
    static void (*deleter())(void*) { return deleter_impl; }
    static void* host_ptr(void* buffer) { return static_cast<MemoryPlacement*>(buffer)->memory.raw(); }
    static size_t size_of(void* buffer) { return static_cast<MemoryPlacement*>(buffer)->memory.size(); }
    static void* factory(size_t bytes) { return new MemoryPlacement(bytes); }
    static size_t default_size() { return functional::SLAB_BYTES; }
    static size_t* type_idx() { static size_t idx = 0; return &idx; }
    static const char* type_name() { return "MemoryContract"; }
    static uint64_t device_address(void* buffer) { return reinterpret_cast<uint64_t>(host_ptr(buffer)); }
    size_t size() const { return memory.size(); }
    void* raw() { return memory.raw(); }
};
static_assert(IsABuffetType<MemoryPlacement>, "MemoryPlacement must satisfy IsABuffetType");
/** --------------------------------------------------------------------------------------------------------- Claim Worker
 * @brief Checks deterministic payload isolation across concurrent claims.
 */
void claim_worker(const BuffetDescriptor* placement, std::atomic<bool>* failed) {
    try {
        for (size_t claim = 0; claim < 1000; ++claim) {
            Slice slice(1024, placement);
            std::memset(slice.raw(), static_cast<unsigned char>(claim), slice.size_bytes());
            if (slice.data<uint8_t>()[0] != static_cast<uint8_t>(claim)
                || slice.data<uint8_t>()[1023] != static_cast<uint8_t>(claim)) {
                failed->store(true, std::memory_order_relaxed);
            }
        }
    } catch (...) {
        failed->store(true, std::memory_order_relaxed);
    }
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Exercises a registered placement through the public Slice and tracking contracts.
 */
int main() {
    require(setenv("ALLIGATOR_GPU_BACKEND", "cpu", 1) == 0, "CPU selection failed");
    const BuffetDescriptor* initial_default = Slice::default_placement();
    const BuffetDescriptor* placement = BuffetDescriptors::descriptor<MemoryPlacement>();
    require(BuffetDescriptors::get(*MemoryPlacement::type_idx()) == placement,
        "registered placement lookup lost its identity");
    require(std::string_view(placement->type_name_fn()) == "MemoryContract",
        "registered placement name differs");
    require(Slice::default_placement() == initial_default, "registration changed the default placement");
    Slice bytes(4096, placement);
    require(sizeof(bytes) == 32 && bytes.placement() == placement, "Slice size or explicit placement differs");
    void* backing = Alligator::buffet_of(bytes);
    require(backing != nullptr, "live Slice lost its backing handle");
    const std::array<unsigned char, 4096> zeros{};
    require(std::memcmp(bytes.raw(), zeros.data(), zeros.size()) == 0, "fresh Slice was not zero initialized");
    Slice probe(1024, placement);
    require(static_cast<char*>(probe.raw()) - static_cast<char*>(bytes.raw()) == 4096,
        "the bump cursor did not advance by the exact claim size");
    bytes.data<uint8_t>()[128] = 91;
    Slice shared = bytes.slice();
    bytes.free();
    require(shared.data<uint8_t>()[128] == 91, "shared Slice did not retain the slab");
    Slice view = shared.slice(64, 256);
    require(Alligator::buffet_of(view) == backing, "subview changed its backing handle");
    require(view.size_bytes() == 256 && view.raw() == shared.data<uint8_t>() + 64,
        "subview changed its byte bounds");
    Slice almost_full(functional::SLAB_BYTES - 8192, placement);
    Slice rollover(16384, placement);
    require(almost_full.valid() && rollover.valid(), "chain rollover failed");
    require(Alligator::buffet_of(rollover) != Alligator::buffet_of(almost_full),
        "the claim past the slab's end stayed on the full link");
    std::atomic<bool> failed{false};
    std::array<std::thread, 8> workers;
    for (std::thread& worker : workers) worker = std::thread(claim_worker, placement, &failed);
    for (std::thread& worker : workers) worker.join();
    require(!failed.load(std::memory_order_relaxed), "concurrent claims overlapped or failed");
    const size_t freed_before = Memory::placement_freed(*placement);
    {
        Slice novel(8192, true, placement);
        require(novel.placement() == placement && novel.is_novel(),
            "novel Slice lost its placement or ownership kind");
        require(Memory::placement_freed(*placement) == freed_before, "novel Slice was freed before its owner");
    }
    require(Memory::placement_freed(*placement) == freed_before + 8192,
        "novel backing was not released exactly once");
    Slice aligned(1024, initial_default);
    require(reinterpret_cast<uintptr_t>(aligned.raw()) % 64 == 0, "default Slice alignment differs");
    require(Memory::placement_usage(*placement) >= placement->default_size_method(),
        "tracker lost live chain backing");
    AtomicRegistry registry;
    AtomicContainer* counter = registry.create<uint64_t>("counter", uint64_t(4));
    require(counter->fetch_add<uint64_t>(3, std::memory_order_relaxed) == 4
        && counter->load<uint64_t>(std::memory_order_relaxed) == 7, "registered atomic arithmetic differs");
    LOG_INFO_STREAM << "Placement registration and Slice ownership contracts passed";
}
