/** --------------------------------------------------------------------------------------------------------- Arena Audit
 * @file contracts.cpp
 * @brief Tests the current library against ownership, allocation, and arena design requirements.
 */
#include "../test_support.hpp"
#include <alligator.hpp>
extern "C" {
#include "core/ba_core.h"
#include "network/ba_network.h"
}
#include "probe.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <semaphore>
#include <string_view>
#include <thread>
#include <vector>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

namespace {
using namespace buffetalligator;
std::atomic<unsigned> placement_allocations{0};
bool misaligned = false;
uint16_t channel_port;
std::binary_semaphore channel_response{0};
bool channel_valid;
/** --------------------------------------------------------------------------------------------------------- Allocate
 * @brief Supplies genuinely allocated aligned storage through the registered placement callback.
 */
ba_handle_t* allocate(size_t bytes, void*) {
    placement_allocations.fetch_add(1, std::memory_order_relaxed);
    void* memory = nullptr;
    if (posix_memalign(&memory, 64, bytes + 64)) return nullptr;
    std::memset(memory, 0, bytes + 64);
    return new ba_handle_t{memory, nullptr};
}
/** --------------------------------------------------------------------------------------------------------- Deallocate
 * @brief Releases the test placement allocation and its handle.
 */
void deallocate(ba_handle_t* handle, void*) {
    std::free(handle->substrate_handle);
    delete handle;
}
/** --------------------------------------------------------------------------------------------------------- Host
 * @brief Returns the actual substrate address or the deliberately invalid alignment fixture.
 */
void* host(ba_handle_t* handle) {
    return static_cast<unsigned char*>(handle->substrate_handle) + (misaligned ? 1 : 0);
}
/** --------------------------------------------------------------------------------------------------------- Description
 * @brief Constructs a bounded test placement descriptor.
 */
ba_placement_desc_t description() {
    ba_placement_desc_t result{};
    result.struct_size = sizeof(result);
    result.name = "arena_audit";
    result.slab_bytes = 4 * 1024 * 1024;
    result.base_alignment = 64;
    result.budget_bytes = 128 * 1024 * 1024;
    result.alloc = allocate;
    result.free = deallocate;
    result.host_ptr = host;
    return result;
}
/** --------------------------------------------------------------------------------------------------------- Register
 * @brief Publishes a test placement through the actual C registry.
 */
uint32_t register_placement() {
    auto descriptor = description();
    uint32_t placement;
    TEST_EQUAL(ba_placement_register(&descriptor, &placement), BA_OK, "placement registration failed");
    return placement;
}
/** --------------------------------------------------------------------------------------------------------- Print Counts
 * @brief Emits observed allocation entry points as machine-readable evidence.
 */
void print_counts(const char* label, const ba_audit_counts& counts) {
    std::printf("%s malloc=%llu calloc=%llu realloc=%llu aligned=%llu map=%llu commit=%llu requested_bytes=%llu\n",
        label, (unsigned long long)counts.malloc_calls, (unsigned long long)counts.calloc_calls,
        (unsigned long long)counts.realloc_calls, (unsigned long long)counts.aligned_calls,
        (unsigned long long)counts.map_calls, (unsigned long long)counts.commit_calls,
        (unsigned long long)counts.requested_bytes);
}
/** --------------------------------------------------------------------------------------------------------- Allocation Calls
 * @brief Counts every instrumented allocation entry point including virtual-memory commitment.
 */
uint64_t allocation_calls(const ba_audit_counts& counts) {
    return counts.malloc_calls + counts.calloc_calls + counts.realloc_calls + counts.aligned_calls
        + counts.map_calls + counts.commit_calls;
}
/** --------------------------------------------------------------------------------------------------------- Layout
 * @brief Reports the current Slice representation without prescribing a replacement layout.
 */
void layout() {
    std::printf("sizeof(Slice)=%zu sizeof(uint32_t)=%zu\n", sizeof(Slice), sizeof(uint32_t));
}
/** --------------------------------------------------------------------------------------------------------- References
 * @brief Reports whether two ranges share the current backing index.
 */
void references() {
    ba_slice_t first, second;
    const uint32_t placement = register_placement();
    TEST_EQUAL(ba_claim(placement, 64, 0, &first), BA_OK, "first claim failed");
    TEST_EQUAL(ba_claim(placement, 64, 0, &second), BA_OK, "second claim failed");
    const auto first_index = static_cast<uint32_t>(first.meta & BA_SLOT_MASK);
    const auto second_index = static_cast<uint32_t>(second.meta & BA_SLOT_MASK);
    std::printf("first_index=%u second_index=%u different_ranges=%d\n",
        first_index, second_index, first.ptr != second.ptr);
    ba_release(&first);
    ba_release(&second);
}
/** --------------------------------------------------------------------------------------------------------- Prepared Claims
 * @brief Confirms warmed small claims perform no library allocation or registry commitment.
 */
void prepared_claims() {
    const uint32_t placement = register_placement();
    ba_slice_t slice;
    TEST_EQUAL(ba_claim(placement, 64, 0, &slice), BA_OK, "warmup failed");
    ba_release(&slice);
    ba_stats_t stats;
    ba_stats(placement, &stats);
    const size_t count = stats.plate_bytes / 64 - 2;
    ba_audit_begin(0);
    for (size_t index = 0; index < count; ++index) {
        TEST_EQUAL(ba_claim(placement, 64, 0, &slice), BA_OK, "prepared claim failed");
        TEST_EQUAL(reinterpret_cast<uintptr_t>(slice.ptr) % 64, 0, "prepared claim alignment failed");
        ba_release(&slice);
    }
    const auto counts = ba_audit_end();
    std::printf("prepared_claims=%zu\n", count);
    print_counts("prepared", counts);
    TEST_EQUAL(allocation_calls(counts), uint64_t{0},
        "Claims within warmed capacity must not allocate or commit registry memory");
}
/** --------------------------------------------------------------------------------------------------------- Prepared Chain
 * @brief Reports initial prepared capacity independently of subsequent rollover policy.
 */
void prepared_chain() {
    const uint32_t placement = register_placement();
    ba_stats_t stats;
    ba_stats(placement, &stats);
    const uint64_t slabs = (stats.slab_bytes_allocated - stats.slab_bytes_freed) / stats.slab_bytes;
    std::printf("initial_slabs=%llu\n", (unsigned long long)slabs);
}
/** --------------------------------------------------------------------------------------------------------- Alignment
 * @brief Observes handling of a callback that violates its own declared alignment precondition.
 */
void alignment() {
    misaligned = true;
    auto descriptor = description();
    uint32_t placement;
    const auto status = ba_placement_register(&descriptor, &placement);
    std::printf("misaligned_registration_status=%s\n", ba_status_name(status));
    if (status == BA_OK) {
        ba_slice_t slice;
        TEST_EQUAL(ba_claim(placement, 65, 0, &slice), BA_OK, "alignment fixture claim failed");
        std::printf("returned_address_mod_64=%zu\n", reinterpret_cast<uintptr_t>(slice.ptr) % 64);
        ba_release(&slice);
    }
}
/** --------------------------------------------------------------------------------------------------------- Registry Commitment
 * @brief Records fresh backing entries crossing the initial writable registry capacity.
 */
void registry_commitment() {
    const uint32_t placement = register_placement();
    const uint32_t before = ba_slot_capacity();
    std::vector<ba_slice_t> slices(before + 16, ba_slice_t{BA_NULL_META, nullptr});
    ba_audit_begin(0);
    for (auto& slice : slices)
        TEST_EQUAL(ba_claim(placement, 64, BA_CLAIM_NOVEL, &slice), BA_OK, "registry fixture claim failed");
    const auto counts = ba_audit_end();
    std::printf("registry_before=%u registry_after=%u claims=%zu\n", before, ba_slot_capacity(), slices.size());
    print_counts("registry", counts);
    for (auto& slice : slices) ba_release(&slice);
}
/** --------------------------------------------------------------------------------------------------------- Cross-thread Lifetime
 * @brief Checks disjoint live ranges and validates their contents after all claiming threads exit.
 */
void cross_thread_lifetime() {
    constexpr size_t threads = 8, per_thread = 4096, bytes = 256;
    const uint32_t placement = register_placement();
    std::vector<ba_slice_t> slices(threads * per_thread);
    std::array<std::thread, threads> workers;
    std::barrier start(static_cast<std::ptrdiff_t>(threads));
    for (size_t worker = 0; worker < threads; ++worker) {
        workers[worker] = std::thread([&, worker] {
            start.arrive_and_wait();
            for (size_t index = worker * per_thread; index < (worker + 1) * per_thread; ++index) {
                TEST_EQUAL(ba_claim(placement, bytes, 0, &slices[index]), BA_OK,
                    "Concurrent claim must succeed while the fixture has available backing");
                TEST_EQUAL(reinterpret_cast<uintptr_t>(slices[index].ptr) % 64, uintptr_t{0},
                    "Every concurrently claimed Slice must start at a 64-byte boundary");
                auto* values = static_cast<uint64_t*>(slices[index].ptr);
                values[0] = index + 1;
                values[bytes / sizeof(uint64_t) - 1] = (index + 1) ^ 0xd394829bull;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    std::vector<uintptr_t> addresses;
    addresses.reserve(slices.size());
    for (size_t index = 0; index < slices.size(); ++index) {
        auto* values = static_cast<uint64_t*>(slices[index].ptr);
        TEST_EQUAL(values[0], index + 1, "Thread exit changed the first word of a live claim");
        TEST_EQUAL(values[bytes / sizeof(uint64_t) - 1], (index + 1) ^ 0xd394829bull,
            "Thread exit changed the last word of a live claim");
        addresses.push_back(reinterpret_cast<uintptr_t>(values));
    }
    std::sort(addresses.begin(), addresses.end());
    for (size_t index = 1; index < addresses.size(); ++index)
        TEST_REQUIRE(addresses[index] >= addresses[index - 1] + bytes, "Live claims overlap");
    for (auto& slice : slices) ba_release(&slice);
    std::printf("verified_disjoint_ranges=%zu threads=%zu bytes=%zu\n", slices.size(), threads, bytes);
}
/** --------------------------------------------------------------------------------------------------------- Wire Allocations
 * @brief Counts real encoder and decoder allocations after warming their placement.
 */
void wire_allocations(bool encrypted) {
    Slice source(1024);
    unsigned char key[32]{}, token[16]{};
    ba_net_frame frame{};
    ba_slice_t result;
    TEST_REQUIRE(!ba_net_encode(reinterpret_cast<const ba_slice_t*>(&source), encrypted ? BA_NET_SECURE : 0,
        token, key, &frame), "wire warmup encode failed");
    TEST_REQUIRE(!ba_net_decode(frame.data, frame.size, encrypted ? 128 : 0, key, &result), "wire warmup decode failed");
    ba_release(&result);
    std::free(frame.data);
    ba_audit_begin(0);
    for (size_t index = 0; index < 1000; ++index) {
        TEST_REQUIRE(!ba_net_encode(reinterpret_cast<const ba_slice_t*>(&source), encrypted ? BA_NET_SECURE : 0,
            token, key, &frame), "wire encode failed");
        TEST_REQUIRE(!ba_net_decode(frame.data, frame.size, encrypted ? 128 : 0, key, &result), "wire decode failed");
        ba_release(&result);
        std::free(frame.data);
    }
    const auto counts = ba_audit_end();
    print_counts(encrypted ? "encrypted_wire_1000" : "plain_wire_1000", counts);
    TEST_EQUAL(allocation_calls(counts), uint64_t{0},
        "After warmup, 1000 Slice encode/decode pairs must not allocate per-message storage");
}
/** --------------------------------------------------------------------------------------------------------- Channel Receive
 * @brief Echoes an actual TCP Slice through the public reply convention.
 */
void channel_receive(Slice slice) {
    SliceChannel::send(std::move(slice), "", channel_port, SliceChannel::Protocol::TCP);
}
/** --------------------------------------------------------------------------------------------------------- Channel Response
 * @brief Publishes validation of one completed network exchange.
 */
void channel_received(Slice slice) {
    channel_valid = slice && slice.size_bytes() == 1024 && slice.data()[0] == 17;
    channel_response.release();
}
/** --------------------------------------------------------------------------------------------------------- Channel Allocations
 * @brief Counts allocations across the actual TCP reactor after listener and exchange warmup.
 */
void channel_allocations() {
    const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
    TEST_REQUIRE(descriptor >= 0, "channel port probe failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_REQUIRE(!bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)), "channel probe bind failed");
    socklen_t length = sizeof(address);
    TEST_REQUIRE(!getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &length), "channel port query failed");
    channel_port = ntohs(address.sin_port);
    ::close(descriptor);
    SliceChannel::listen(channel_port, SliceChannel::Protocol::TCP, channel_receive);
    Slice source(1024);
    source.data()[0] = 17;
    for (size_t index = 0; index < 132; ++index) {
        if (index == 32) ba_audit_begin(1);
        SliceChannel::send(source, "127.0.0.1", channel_port, SliceChannel::Protocol::TCP, channel_received);
        TEST_REQUIRE(channel_response.try_acquire_for(std::chrono::seconds(5)), "channel response timed out");
        TEST_REQUIRE(channel_valid, "channel payload changed");
    }
    SliceChannel::close(channel_port, SliceChannel::Protocol::TCP);
    const auto counts = ba_audit_end();
    print_counts("tcp_round_trips_100", counts);
    TEST_EQUAL(allocation_calls(counts), uint64_t{0},
        "After 32 warmup exchanges, 100 TCP round trips must not allocate per-message storage");
}
/** --------------------------------------------------------------------------------------------------------- Queue Identity
 * @brief Checks descriptor identity, payload visibility, and zero library allocations during handoff.
 */
void queue_identity() {
    SliceQueue queue(1, 1);
    constexpr size_t count = 8192;
    std::vector<Slice> slices;
    std::vector<std::array<unsigned char, sizeof(Slice)>> descriptors(count);
    slices.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        slices.emplace_back(64);
        slices.back().data<uint64_t>()[0] = index;
        std::memcpy(descriptors[index].data(), &slices.back(), sizeof(Slice));
    }
    std::binary_semaphore ready{0}, begin{0}, drained{0}, finish{0};
    std::atomic<bool> valid{true};
    std::thread consumer([&] {
        auto reader = queue.consumer(0);
        ready.release();
        begin.acquire();
        for (size_t index = 0; index < count; ++index) {
            Slice received;
            if (!reader.pop(received) || received.data<uint64_t>()[0] != index ||
                std::memcmp(&received, descriptors[index].data(), sizeof(Slice))) valid.store(false);
        }
        drained.release();
        finish.acquire();
    });
    auto writer = queue.producer(0);
    ready.acquire();
    ba_audit_begin(1);
    begin.release();
    for (auto& slice : slices) writer.push(std::move(slice));
    writer.flush();
    drained.acquire();
    const auto counts = ba_audit_end();
    finish.release();
    consumer.join();
    print_counts("queue_8192", counts);
    TEST_REQUIRE(valid.load(), "Queue changed descriptor identity or payload");
    TEST_EQUAL(allocation_calls(counts), uint64_t{0},
        "Moving 8192 preclaimed Slices through an initialized queue must not allocate");
}
/** --------------------------------------------------------------------------------------------------------- Typed Count
 * @brief Checks the preserved count constructor including multiplication overflow.
 */
void typed_count() {
    SliceT<uint64_t> values(size_t{19});
    TEST_EQUAL(values.size_bytes(), 19 * sizeof(uint64_t), "SliceT count constructor size is wrong");
    bool rejected = false;
    try { SliceT<uint64_t> oversized(SIZE_MAX / sizeof(uint64_t) + 1); }
    catch (const AlligatorException&) { rejected = true; }
    TEST_REQUIRE(rejected, "SliceT count multiplication overflow was accepted");
}
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs one isolated contract so failures cannot suppress subsequent cases.
 */
int main(int count, char** arguments) {
    test_support::start(__FILE__);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    try {
        TEST_EQUAL(count, 2, "Specify an audit case");
        const std::string_view selected(arguments[1]);
        test_support::step(arguments[1]);
        if (selected == "layout") layout();
        else if (selected == "references") references();
        else if (selected == "prepared_claims") prepared_claims();
        else if (selected == "prepared_chain") prepared_chain();
        else if (selected == "alignment") alignment();
        else if (selected == "registry_commitment") registry_commitment();
        else if (selected == "cross_thread_lifetime") cross_thread_lifetime();
        else if (selected == "plain_wire") wire_allocations(false);
        else if (selected == "encrypted_wire") wire_allocations(true);
        else if (selected == "channel_allocations") channel_allocations();
        else if (selected == "queue_identity") queue_identity();
        else if (selected == "typed_count") typed_count();
        else throw std::runtime_error("Unknown audit case");
        const bool observation = selected == "layout" || selected == "references" ||
            selected == "prepared_chain" || selected == "alignment" || selected == "registry_commitment";
        std::printf("%s %s\n", observation ? "OBSERVATION" : "PASS", arguments[1]);
        BuffetMenu::shutdown();
        return 0;
    } catch (const std::exception& error) {
        test_support::fail("Unexpected exception; source is the last test checkpoint",
            test_support::last_operation, "successful completion", error.what(), test_support::last_location);
    }
}
