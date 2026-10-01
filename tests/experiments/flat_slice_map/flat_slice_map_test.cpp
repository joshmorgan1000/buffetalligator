/** --------------------------------------------------------------------------------------------------------- Flat Slice Map Ownership
 * @file flat_slice_map_test.cpp
 * @brief Tests retained ownership and published read phases through the experimental flat map.
 */
#include "flat_slice_map.hpp"
#include "../../functional_support.hpp"
#include <array>
#include <atomic>
#include <functional>
#include <limits>
#include <thread>
#include <utility>

namespace {
using buffetalligator::AlignedHeapBuffer;
using buffetalligator::BuffetDescriptor;
using buffetalligator::BuffetDescriptors;
using buffetalligator::Slice;
using experiments::FlatSliceMap;
using functional::require;
std::atomic<size_t> released_payloads{0};
/** --------------------------------------------------------------------------------------------------------- Tracked Factory
 * @brief Allocates real host backing for the public test placement.
 */
void* tracked_factory(size_t bytes) { return new AlignedHeapBuffer(bytes); }
/** --------------------------------------------------------------------------------------------------------- Tracked Deleter
 * @brief Counts dedicated test payload release separately from the placement's slab roots.
 */
void tracked_deleter(void* pointer) {
    auto* backing = static_cast<AlignedHeapBuffer*>(pointer);
    if (backing->size() == 192) released_payloads.fetch_add(1, std::memory_order_relaxed);
    delete backing;
}
const BuffetDescriptor tracked_placement{
    "FlatMapOwnership", tracked_deleter, AlignedHeapBuffer::host_ptr,
    AlignedHeapBuffer::size_of, tracked_factory, 2, 4096, AlignedHeapBuffer::device_address
};
/** --------------------------------------------------------------------------------------------------------- Payload
 * @brief Creates a dedicated three-granule payload with observable ownership and view contents.
 */
Slice payload(uint64_t value) {
    Slice result(192, true, &tracked_placement);
    result.data<uint64_t>()[0] = value;
    result.data<uint64_t>()[8] = value + 1;
    result.data<uint64_t>()[16] = value + 2;
    return result;
}
/** --------------------------------------------------------------------------------------------------------- Heap Payload
 * @brief Creates a single-granule row for deterministic growth and reader workloads.
 */
Slice heap_payload(uint64_t value) {
    Slice result(64, BuffetDescriptors::descriptor_for(static_cast<AlignedHeapBuffer*>(nullptr)));
    result.get_as<uint64_t>() = value;
    return result;
}
/** --------------------------------------------------------------------------------------------------------- Null And Keys
 * @brief Verifies misses, null replacements, signed key extremes, and release of unretained claims.
 */
void null_and_keys() {
    LOG_INFO_STREAM << "Checking flat map missing keys, null rows, and signed key extremes";
    FlatSliceMap map(0);
    require(map.size() == 0 && !map.get_slice(-1), "empty flat map lookup was not null");
    map.add_slice(-1, Slice());
    require(map.size() == 1 && !map.get_slice(-1), "null payload did not retain its key");
    const size_t before = released_payloads.load(std::memory_order_relaxed);
    map.add_slice(-1, payload(101));
    require(map.size() == 1 && map.get_slice(-1).get_as<uint64_t>() == 101,
        "non-null replacement failed or added another key");
    map.add_slice(-1, payload(202));
    require(released_payloads.load(std::memory_order_relaxed) == before + 1,
        "replacement retained the previous unshared backing");
    map.add_slice(-1, Slice());
    require(map.size() == 1 && !map.get_slice(-1)
        && released_payloads.load(std::memory_order_relaxed) == before + 2,
        "null replacement did not release the previous backing");
    constexpr int64_t minimum = std::numeric_limits<int64_t>::min();
    constexpr int64_t maximum = std::numeric_limits<int64_t>::max();
    map.add_slice(minimum, heap_payload(303));
    map.add_slice(maximum, heap_payload(404));
    map.add_slice(0, heap_payload(505));
    require(map.size() == 4 && map.get_slice(minimum).get_as<uint64_t>() == 303
        && map.get_slice(maximum).get_as<uint64_t>() == 404
        && map.get_slice(0).get_as<uint64_t>() == 505 && !map.get_slice(-2),
        "flat map confused signed extremes, zero, or a missing key");
    map.reset();
    require(map.size() == 0 && !map.get_slice(minimum), "reset did not erase all keys");
}
/** --------------------------------------------------------------------------------------------------------- Retained Ownership
 * @brief Checks independent IDs and metadata through growth, replacement, reset, and destruction.
 */
void retained_ownership() {
    LOG_INFO_STREAM << "Checking flat map retained ownership across growth and mutation";
    const size_t before = released_payloads.load(std::memory_order_relaxed);
    Slice original_result;
    Slice original_peer;
    Slice replacement_result;
    Slice destruction_result;
    {
        FlatSliceMap map(1);
        Slice original = payload(1001);
        const uint32_t inserted_identifier = original.id();
        const void* inserted_pointer = original.raw();
        map.add_slice(-7, std::move(original));
        require(!original, "flat map insertion did not move the caller's ownership");
        original_result = map.get_slice(-7);
        original_peer = map.get_slice(-7);
        require(original_result.id() != inserted_identifier
            && original_peer.id() != inserted_identifier
            && original_result.id() != original_peer.id()
            && original_result.raw() == inserted_pointer && original_peer.raw() == inserted_pointer,
            "retained flat map lookup did not create a distinct zero-copy Slice");
        original_result.resize(64, true, false, &tracked_placement);
        require(original_result.size_bytes() == 64 && original_peer.size_bytes() == 192
            && map.get_slice(-7).size_bytes() == 192,
            "changing retained view metadata changed another claim");
        constexpr size_t growth_rows = 4096;
        for (size_t index = 0; index < growth_rows; ++index) {
            map.add_slice(static_cast<int64_t>(index), heap_payload(index));
        }
        require(map.size() == growth_rows + 1 && original_peer.data<uint64_t>()[16] == 1003
            && map.get_slice(-7).raw() == inserted_pointer,
            "flat table growth invalidated a retained or stored payload");
        for (size_t index = 0; index < growth_rows; ++index) {
            require(map.get_slice(static_cast<int64_t>(index)).get_as<uint64_t>() == index,
                "flat table growth lost a published key");
        }
        map.add_slice(-7, payload(2001));
        replacement_result = map.get_slice(-7);
        require(map.size() == growth_rows + 1 && replacement_result.get_as<uint64_t>() == 2001
            && original_result.get_as<uint64_t>() == 1001
            && released_payloads.load(std::memory_order_relaxed) == before,
            "replacement changed a retained value or released its backing early");
        map.reset();
        require(map.size() == 0 && !map.get_slice(-7)
            && replacement_result.data<uint64_t>()[16] == 2003
            && released_payloads.load(std::memory_order_relaxed) == before,
            "reset invalidated a retained result");
        map.add_slice(-7, payload(3001));
        destruction_result = map.get_slice(-7);
        require(map.size() == 1, "flat map could not be reused after reset");
    }
    require(destruction_result.data<uint64_t>()[16] == 3003
        && original_peer.data<uint64_t>()[8] == 1002
        && released_payloads.load(std::memory_order_relaxed) == before,
        "map destruction invalidated an independently retained result");
    original_result.free();
    require(released_payloads.load(std::memory_order_relaxed) == before,
        "retiring one result released backing retained by another result");
    original_peer.free();
    replacement_result.free();
    destruction_result.free();
    require(released_payloads.load(std::memory_order_relaxed) == before + 3,
        "retained payload backings were not released exactly once");
}
/** --------------------------------------------------------------------------------------------------------- Published Reader
 * @brief Exercises only const lookup after the owner has published the complete table.
 */
void published_reader(const FlatSliceMap& map, size_t worker) {
    constexpr size_t rows = 128;
    for (size_t round = 0; round < 512; ++round) {
        const size_t identifier = (round + worker) % rows;
        Slice retained = map.get_slice(static_cast<int64_t>(identifier));
        require(retained && retained.get_as<uint64_t>() == identifier,
            "published concurrent const lookup returned the wrong payload");
    }
}
/** --------------------------------------------------------------------------------------------------------- Published Readers
 * @brief Confines mutations to phases before reader creation and after all readers join.
 */
void published_readers() {
    LOG_INFO_STREAM << "Checking flat map concurrent readers after exclusive publication";
    FlatSliceMap map(128);
    for (size_t index = 0; index < 128; ++index) {
        map.add_slice(static_cast<int64_t>(index), heap_payload(index));
    }
    std::array<std::thread, 4> readers;
    for (size_t worker = 0; worker < readers.size(); ++worker) {
        readers[worker] = std::thread(published_reader, std::cref(map), worker);
    }
    for (auto& reader : readers) reader.join();
    map.reset();
    require(map.size() == 0, "exclusive reset after reader join did not erase the table");
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs the flat map ownership checks using the public Slice ABI.
 */
int main() {
    static_assert(sizeof(Slice) == 4);
    BuffetDescriptors::register_descriptor(&tracked_placement);
    null_and_keys();
    retained_ownership();
    published_readers();
    LOG_INFO_STREAM << "Flat Slice map ownership and published-reader checks passed";
}
