/** --------------------------------------------------------------------------------------------------------- Global Slice Startup Test
 * @file global_slice_startup_test.cpp
 * @brief Verifies CPU-first global Slice lifetime without including the executor header.
 */
#include <alligator.hpp>
#include <alligator/atomics.hpp>
#include <alligator/easygpu.hpp>
#include <memory/lifetime.hpp>
#include <atomic>
#include <cstdlib>

using namespace buffetalligator;
namespace {
constinit std::atomic<size_t> allocations{0};
constinit std::atomic<size_t> releases{0};
/** --------------------------------------------------------------------------------------------------------- Allocate
 * @brief Counts real heap backing created by production Slice reservations.
 */
void* allocate(size_t bytes) {
    void* buffer = AlignedHeapBuffer::factory(bytes);
    allocations.fetch_add(1, std::memory_order_relaxed);
    return buffer;
}
/** --------------------------------------------------------------------------------------------------------- Release
 * @brief Counts final backing reclamation from global handles and retained slab roots.
 */
void release(void* buffer) {
    releases.fetch_add(1, std::memory_order_relaxed);
    AlignedHeapBuffer::deleter_impl(buffer);
}
const BuffetDescriptor placement{"GlobalStartup", &release, &AlignedHeapBuffer::host_ptr_impl,
    &AlignedHeapBuffer::size_of_impl, &allocate, 7, 256, &AlignedHeapBuffer::device_address};
/** --------------------------------------------------------------------------------------------------------- Verify Exit
 * @brief Checks final reclamation after arena teardown without using an already-destroyed logger.
 */
void verify_exit(void*) {
    if (allocations.load(std::memory_order_relaxed) == 0
        || allocations.load(std::memory_order_relaxed) != releases.load(std::memory_order_relaxed))
        std::_Exit(EXIT_FAILURE);
}
RuntimeFinalizer final_verification(nullptr, &verify_exit);
/** --------------------------------------------------------------------------------------------------------- Global Owner
 * @brief Constructs arena-backed state before main and releases it before arena shutdown.
 */
struct GlobalOwner {
    Slice dedicated;
    Slice view;
    Slice chained;
    GlobalOwner() {
        BuffetDescriptors::register_descriptor(&placement);
        dedicated = Slice(193, true, &placement);
        dedicated.data<uint64_t>()[8] = 0x123456789abcdef0ull;
        view = dedicated.slice(65, 65);
        chained = Slice(64, &placement);
        chained.get_as<uint32_t>() = 137;
        Slice registered(64, true, &placement);
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
