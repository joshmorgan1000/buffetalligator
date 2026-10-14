/** --------------------------------------------------------------------------------------------------------- Buffet Alligator
 * @file alligator.cpp
 * @brief Implements the slot and buffer tables behind Slice ids and the arena's lifetime; the
 * thread pool lives in the Kitchen.
 */
#include <alligator.hpp>
#include <memory/arena.hpp>
#include <memory/lifetime.hpp>
#include <gpu/runtime.hpp>
#include <vulkan/shader_state.hpp>
#include <bit>
#include <cstdlib>
#include <new>
extern "C" void ba_net_shutdown(void);

namespace buffetalligator {
static_assert(DIRECTORY_GRANULES * sizeof(uint64_t) == 526336,
    "The GLSL and Metal preludes read buffer granule counts at directory byte 526336");
/** --------------------------------------------------------------------------------------------------------- Extent Of
 * @brief Picks the smallest unit whose count fits the 20-bit length field.
 */
Extent Extent::of(uint64_t granules) {
    for (uint64_t scale = 0; scale < 4; ++scale) {
        const uint64_t units = (granules + (uint64_t{1} << (2 * scale)) - 1) >> (2 * scale);
        if (units <= UNITS_LIMIT) return {scale, units, units << (2 * scale)};
    }
    return {0, 0, granules};
}
/** --------------------------------------------------------------------------------------------------------- Open
 * @brief Allocates a buffer, claims a free index, and waits out claimers undoing a stale count.
 */
uint32_t Arena::open(const BuffetDescriptor* placement, uint64_t granules, bool novel) {
    static thread_local uint64_t cursor = 0;
    void* native = placement->factory(granules << 6);
    for (uint64_t scanned = 0, index = cursor; scanned < BUFFER_COUNT / 64;
        ++scanned, index = (index + 1) & (BUFFER_COUNT / 64 - 1)) {
        std::atomic_ref<uint64_t> word(buffer_bits[index]);
        uint64_t bits = word.load(std::memory_order_relaxed);
        while (~bits) {
            const uint64_t bit = uint64_t{1} << std::countr_one(bits);
            bits = word.fetch_or(bit, std::memory_order_acquire);
            if (bits & bit) continue;
            cursor = index;
            const uint32_t buffer = static_cast<uint32_t>(index * 64 + std::countr_zero(bit));
            BufferRecord& record = buffers[buffer];
            record.host_base = static_cast<uint8_t*>(placement->host_ptr(native, 0));
            record.native = native;
            record.granules = static_cast<uint32_t>(granules);
            record.placement = placement->type_idx;
            record.novel = novel;
            directory[SEGMENT_COUNT + buffer] = placement->device_address(native);
            reinterpret_cast<uint32_t*>(directory + DIRECTORY_GRANULES)[buffer] = record.granules;
            std::atomic_ref<uint32_t>(successors[buffer]).store(BUFFER_NONE, std::memory_order_relaxed);
            std::atomic_ref<uint64_t> live(record.cursor);
            uint64_t expected = live.load(std::memory_order_relaxed);
            do {
                while (expected >= LIVE_ONE) expected = live.load(std::memory_order_relaxed);
            } while (!live.compare_exchange_weak(expected, LIVE_ONE, std::memory_order_release,
                std::memory_order_relaxed));
            return buffer;
        }
    }
    placement->deleter(native);
    ALLIGATOR_THROW("All 65,536 buffer indices are in use");
}
/** --------------------------------------------------------------------------------------------------------- Publish
 * @brief Scans from this thread's last slot word and publishes a segment when all are taken.
 */
uint32_t Arena::publish(uint64_t word, uint8_t type) {
    static std::atomic<uint64_t> spread{0};
    static thread_local uint64_t cursor = spread.fetch_add(61, std::memory_order_relaxed);
    for (;;) {
        const uint32_t count = segments.load(std::memory_order_acquire);
        const uint64_t words = uint64_t{count} * SEGMENT_WORDS;
        const uint64_t start = cursor < words ? cursor : words ? cursor % words : 0;
        for (uint64_t scanned = 0, index = start; scanned < words;
            ++scanned, index = index + 1 == words ? 0 : index + 1) {
            std::atomic_ref<uint64_t> bits(
                occupancy[index / SEGMENT_WORDS][index % SEGMENT_WORDS]);
            uint64_t current = bits.load(std::memory_order_relaxed);
            while (~current) {
                const uint64_t bit = uint64_t{1} << std::countr_one(current);
                current = bits.fetch_or(bit, std::memory_order_acquire);
                if (current & bit) continue;
                cursor = index;
                const uint32_t slot = static_cast<uint32_t>(index * 64 + std::countr_zero(bit));
                locations[slot >> SEGMENT_BITS][slot & SEGMENT_MASK] = word;
                states[slot >> SEGMENT_BITS][slot & SEGMENT_MASK] = {1, type, 0};
                return slot;
            }
        }
        std::lock_guard lock(growth);
        if (segments.load(std::memory_order_relaxed) != count) continue;
        if (count == SEGMENT_COUNT) ALLIGATOR_THROW("All 16,777,216 Slice slots are in use");
        void* buffet = metadata->factory(SEGMENT_SLOTS * sizeof(uint64_t));
        auto* host = static_cast<unsigned char*>(
            std::calloc(1, SEGMENT_SLOTS * sizeof(SlotState) + SEGMENT_WORDS * sizeof(uint64_t)));
        if (!host) {
            metadata->deleter(buffet);
            throw std::bad_alloc();
        }
        segment_buffets[count] = buffet;
        locations[count] = static_cast<uint64_t*>(metadata->host_ptr(buffet, 0));
        states[count] = reinterpret_cast<SlotState*>(host);
        occupancy[count] = reinterpret_cast<uint64_t*>(host + SEGMENT_SLOTS * sizeof(SlotState));
        directory[count] = metadata->device_address(buffet);
        cursor = uint64_t{count} * SEGMENT_WORDS;
        segments.store(count + 1, std::memory_order_release);
    }
}
/** --------------------------------------------------------------------------------------------------------- Retire
 * @brief Releases the backing allocation and its index once no slot or chain holds the buffer.
 */
void Arena::retire(uint32_t buffer) {
    BufferRecord& record = buffers[buffer];
    if (std::atomic_ref<uint64_t>(record.cursor).fetch_sub(LIVE_ONE, std::memory_order_acq_rel)
        >= 2 * LIVE_ONE) return;
    BuffetDescriptors::get(record.placement)->deleter(record.native);
    std::atomic_ref<uint64_t>(buffer_bits[buffer / 64]).fetch_and(~(uint64_t{1} << (buffer % 64)),
        std::memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Constructor
 * @brief Allocates the GPU-visible directory; slot segments are published on first claim.
 */
Alligator::Alligator() {
    Arena::metadata = gpu_device().placement;
    Arena::directory_buffet = Arena::metadata->factory(DIRECTORY_WORDS * sizeof(uint64_t));
    Arena::directory = static_cast<uint64_t*>(
        Arena::metadata->host_ptr(Arena::directory_buffet, 0));
    Arena::directory_address = Arena::metadata->device_address(Arena::directory_buffet);
}
/** --------------------------------------------------------------------------------------------------------- Destructor
 * @brief Drains accepted work before releasing chain holds and the slot tables.
 */
Alligator::~Alligator() {
    ba_net_shutdown();
    ShaderState::drain();
    ChainBuffet::release_chains();
    const uint32_t count = Arena::segments.exchange(0, std::memory_order_acq_rel);
    for (uint32_t segment = 0; segment < count; ++segment) {
        Arena::metadata->deleter(Arena::segment_buffets[segment]);
        std::free(Arena::states[segment]);
        Arena::segment_buffets[segment] = nullptr;
        Arena::locations[segment] = nullptr;
        Arena::states[segment] = nullptr;
        Arena::occupancy[segment] = nullptr;
    }
    Arena::metadata->deleter(Arena::directory_buffet);
    Arena::directory_buffet = nullptr;
    Arena::directory = nullptr;
    Arena::directory_address = 0;
}
/** ------------------------------------------------------------------------------------------- Instance
 * @brief Returns the singleton instance of the Alligator.
 * @return The Alligator instance.
 */
Alligator& Alligator::inst() {
    static RuntimeFinalizer lifetime(new Alligator, &RuntimeFinalizer::delete_owner<Alligator>,
        RuntimeFinalizer::Phase::Arena);
    return *static_cast<Alligator*>(lifetime.object);
}
/** --------------------------------------------------------------------------------------------------------- Device Address
 * @brief Adds the slot's granule offset to its buffer's device base, which is zero off the device.
 */
uint64_t Alligator::device_address(const Slice& slice) {
    if (slice.is_null()) return 0;
    const uint32_t slot = slice.id();
    const uint64_t word = Arena::locations[slot >> SEGMENT_BITS][slot & SEGMENT_MASK];
    const uint64_t base = Arena::directory[SEGMENT_COUNT + Location::buffer(word)];
    return base == 0 ? 0 : base + (Location::offset(word) << 6);
}
/** --------------------------------------------------------------------------------------------------------- GPU Directory
 * @brief Returns the device address of the arena directory.
 */
uint64_t Alligator::gpu_directory_address() {
    (void)inst();
    return Arena::directory_address;
}
} // namespace buffetalligator
