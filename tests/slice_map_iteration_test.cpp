/** --------------------------------------------------------------------------------------------------------- Slice Map Iteration Test
 * @file slice_map_iteration_test.cpp
 * @brief Checks ordered ID snapshots and concurrent callbacks during replacement, reset, and reentry.
 */
#include <alligator.hpp>
#include <alligator/containers.hpp>
#include <alligator/dispatch.hpp>
#include "functional_support.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace buffetalligator;
using functional::require;
namespace {
/** --------------------------------------------------------------------------------------------------------- Payload
 * @brief Creates one deterministic scalar payload through the public allocation interface.
 */
Slice payload(int64_t value) {
    Slice result(sizeof(value));
    result.get_as<int64_t>() = value;
    return result;
}
/** --------------------------------------------------------------------------------------------------------- Converted ID
 * @brief Exercises an implicitly convertible identifier without a default constructor.
 */
struct ConvertedId {
    int64_t value;
    ConvertedId(int64_t identifier) : value(identifier) {}
};
/** --------------------------------------------------------------------------------------------------------- Rvalue ID
 * @brief Exercises conversion that accepts the native identifier only as an rvalue.
 */
struct RvalueId {
    int64_t value;
    RvalueId(int64_t&& identifier) : value(identifier) {}
};
/** --------------------------------------------------------------------------------------------------------- Observations
 * @brief Stores callback results independently of concurrent invocation order.
 */
struct Observations {
    std::unordered_map<int64_t, int64_t> rows;
    std::mutex mutex;
    bool replace_handle = false;
};
/** --------------------------------------------------------------------------------------------------------- Observe
 * @brief Records callback contents and optionally replaces only the callback's local Slice handle.
 */
void observe(int64_t identifier, Slice* slice, void* context) {
    auto& observations = *static_cast<Observations*>(context);
    {
        std::lock_guard lock(observations.mutex);
        require(observations.rows.emplace(identifier,
            slice->is_null() ? -999 : slice->get_as<int64_t>()).second,
            "traversal invoked a row more than once");
    }
    if (observations.replace_handle) *slice = payload(-777);
}
/** --------------------------------------------------------------------------------------------------------- Boundaries
 * @brief Checks empty maps, signed identifiers, growth boundaries, conversion, and local handle isolation.
 */
void boundaries() {
    SliceMap map(0);
    const SliceMap& readable = map;
    Observations empty;
    require(readable.ids().empty() && readable.ids<ConvertedId>().empty(),
        "empty identifier snapshot contained rows");
    readable.for_each(&observe, &empty);
    require(empty.rows.empty(), "empty traversal invoked its callback");
    constexpr std::array<int64_t, 11> special{
        0, -1, INT64_MIN, INT64_MAX, 7, -2, 31, UINT32_MAX, 8, 16, 15
    };
    constexpr size_t count = 4097;
    std::vector<int64_t> expected;
    expected.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const int64_t identifier = index < special.size() ? special[index]
            : static_cast<int64_t>(100000 + index);
        expected.push_back(identifier);
        map.add_slice(identifier, index == 0 ? Slice() : payload(static_cast<int64_t>(index)));
        if ((index & (index + 1)) == 0) {
            require(readable.ids() == expected, "segment boundary changed identifier order");
        }
    }
    map.add_slice(expected[1023], payload(8191));
    require(readable.ids() == expected, "replacement changed snapshot position or count");
    const auto converted = readable.ids<ConvertedId>();
    const auto rvalues = readable.ids<RvalueId>();
    const auto unsigned_ids = readable.ids<uint64_t>();
    require(converted.size() == count && rvalues.size() == count && unsigned_ids.size() == count,
        "identifier conversion changed snapshot size");
    for (size_t index = 0; index < count; ++index) {
        require(converted[index].value == expected[index]
            && rvalues[index].value == expected[index]
            && unsigned_ids[index] == static_cast<uint64_t>(expected[index]),
            "identifier conversion changed the stored value");
    }
    Observations observations;
    observations.replace_handle = true;
    readable.for_each(&observe, &observations);
    require(observations.rows.size() == expected.size(), "traversal lost published rows");
    for (size_t index = 0; index < count; ++index) {
        const int64_t expected_value = index == 0 ? -999
            : index == 1023 ? 8191 : static_cast<int64_t>(index);
        require(observations.rows.at(expected[index]) == expected_value,
            "traversal selected another row's payload");
        Slice retained = map.slice_at(index);
        require(index == 0 ? retained.is_null() : retained.get_as<int64_t>() == expected_value,
            "reassigning the callback Slice replaced map storage");
    }
}
/** --------------------------------------------------------------------------------------------------------- Publication Context
 * @brief Holds a first insertion inside its publication hook while a later insertion finishes.
 */
struct PublicationContext {
    SliceMap& map;
    std::barrier<> phase{2};
};
/** --------------------------------------------------------------------------------------------------------- Delayed Publication
 * @brief Delays only the second stable position before its hook completion is announced.
 */
void delayed_publication(void*, void* context, const size_t& index) {
    if (index != 1) return;
    auto& state = *static_cast<PublicationContext*>(context);
    state.phase.arrive_and_wait();
    state.phase.arrive_and_wait();
}
/** --------------------------------------------------------------------------------------------------------- Publish Delayed
 * @brief Inserts the controlled incomplete row on another thread.
 */
void publish_delayed(PublicationContext* context) {
    context->map.add_slice(20, payload(20));
}
/** --------------------------------------------------------------------------------------------------------- Publication Prefix
 * @brief Excludes incomplete and later rows until the contiguous publication prefix advances.
 */
void publication_prefix() {
    SliceMap map(1);
    map.add_slice(10, payload(10));
    PublicationContext context{map};
    map.on_publish(&delayed_publication, &context);
    std::thread publisher(&publish_delayed, &context);
    context.phase.arrive_and_wait();
    map.add_slice(30, payload(30));
    require(map.ids() == std::vector<int64_t>{10},
        "identifier snapshot included a hook-incomplete prefix");
    Observations observations;
    map.for_each(&observe, &observations);
    require(observations.rows.size() == 1 && observations.rows.at(10) == 10,
        "traversal included a row beyond the hook-completed prefix");
    context.phase.arrive_and_wait();
    publisher.join();
    require(map.ids() == std::vector<int64_t>{10, 20, 30},
        "completed publication did not release the remaining snapshot prefix");
}
/** --------------------------------------------------------------------------------------------------------- Throwing Context
 * @brief Counts callbacks independently of dispatch order.
 */
struct ThrowingContext {
    std::atomic<size_t> visits{0};
};
/** --------------------------------------------------------------------------------------------------------- Throwing Callback
 * @brief Throws from one known row to verify dispatch joins before propagating a callback failure.
 */
void throwing_callback(int64_t identifier, Slice*, void* context) {
    static_cast<ThrowingContext*>(context)->visits.fetch_add(1, std::memory_order_relaxed);
    if (identifier == 2) {
        throw std::runtime_error("iteration sentinel");
    }
}
/** --------------------------------------------------------------------------------------------------------- Exceptions
 * @brief Checks that a callback exception reaches the caller and leaves the map usable.
 */
void exceptions() {
    SliceMap map(4);
    for (int64_t identifier = 0; identifier < 4; ++identifier) {
        map.add_slice(identifier, payload(identifier));
    }
    ThrowingContext context;
    bool caught = false;
    try {
        map.for_each(&throwing_callback, &context);
    } catch (const std::runtime_error& error) {
        caught = std::string_view(error.what()) == "iteration sentinel";
    }
    require(caught && context.visits.load() >= 3 && context.visits.load() <= 4,
        "callback exception was lost or dispatch skipped unrelated worker ranges");
    map.reset();
    map.add_slice(9, payload(19));
    Observations observed;
    map.for_each(&observe, &observed);
    require(observed.rows.size() == 1 && observed.rows.at(9) == 19,
        "callback exception left a stale traversal state");
    SliceMap::gc();
}
/** --------------------------------------------------------------------------------------------------------- Owned Value
 * @brief Counts finalization independently of the Slice allocation's retained reference count.
 */
struct OwnedValue {
    int64_t value;
    std::atomic<size_t>& destructions;
    OwnedValue(int64_t identifier, std::atomic<size_t>& released)
    : value(identifier), destructions(released) {}
    ~OwnedValue() { destructions.fetch_add(1, std::memory_order_relaxed); }
};
/** --------------------------------------------------------------------------------------------------------- Count Typed
 * @brief Reads a typed payload from recursive traversal and counts each callback.
 */
void count_typed(int64_t, Slice* slice, void* context) {
    require(slice->get_as<OwnedValue>().value >= 10, "nested traversal lost its typed payload");
    static_cast<std::atomic<size_t>*>(context)->fetch_add(1, std::memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Count Values
 * @brief Reads a plain payload from recursive dispatch and counts its callback.
 */
void count_values(int64_t, Slice* slice, void* context) {
    require(slice->get_as<int64_t>() >= 10, "nested traversal lost its payload");
    static_cast<std::atomic<size_t>*>(context)->fetch_add(1, std::memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Reentry Context
 * @brief Couples recursive operations to the outer callback's captured row generation.
 */
struct ReentryContext {
    SliceMap& map;
    SliceMap& pin_target;
    std::atomic<size_t> visits{0};
    bool throw_after_reset = false;
};
/** --------------------------------------------------------------------------------------------------------- Reenter
 * @brief Replaces and resets the map while recursively using every hazard-taking read interface.
 */
void reenter(int64_t identifier, Slice* slice, void* context) {
    auto& state = *static_cast<ReentryContext*>(context);
    state.visits.fetch_add(1, std::memory_order_relaxed);
    require(slice->get_as<int64_t>() == identifier,
        "outer traversal switched generations after recursive reset");
    if (identifier != 10) return;
    state.map.add_slice(10, payload(110));
    require(state.map.size() == 2 && state.map.find(10) == 0,
        "reentrant replacement changed row identity");
    require(state.map.get_slice(10).get_as<int64_t>() == 110,
        "reentrant lookup did not see replacement");
    require(state.map.ids() == std::vector<int64_t>{10, 20},
        "reentrant identifier snapshot changed order");
    std::atomic<size_t> nested_visits{0};
    state.map.for_each(&count_values, &nested_visits);
    require(nested_visits.load() == 2, "recursive traversal lost rows");
    const int64_t* active = slice->data<int64_t>();
    *slice = Slice();
    state.map.reset();
    state.map.add_slice(90, payload(190));
    require(state.map.find(90) == 0, "reset map lost its replacement generation");
    state.pin_target.find(-1);
    SliceMap::gc();
    require(*active == 10, "nested operations reclaimed the active callback's payload");
    if (state.throw_after_reset) throw std::runtime_error("typed iteration sentinel");
}
/** --------------------------------------------------------------------------------------------------------- Reentry
 * @brief Checks that recursive parallel callbacks and exceptions retain their captured generation.
 */
void reentry(bool throw_after_reset) {
    SliceMap map(2);
    SliceMap pin_target(0);
    map.add_slice(10, payload(10));
    map.add_slice(20, payload(20));
    ReentryContext context{map, pin_target};
    context.throw_after_reset = throw_after_reset;
    bool caught = false;
    try {
        map.for_each(&reenter, &context);
    } catch (const std::runtime_error& error) {
        caught = std::string_view(error.what()) == "typed iteration sentinel";
    }
    require(caught == throw_after_reset && context.visits.load() >= 1
        && context.visits.load() <= 2 && (throw_after_reset || context.visits.load() == 2),
        "reset or exception changed the captured generation's traversal bounds");
    require(map.ids() == std::vector<int64_t>{90}, "callback reset lost the new generation");
}
/** --------------------------------------------------------------------------------------------------------- Concurrent Context
 * @brief Coordinates exact writer interleavings with the callback's active typed payload.
 */
struct ConcurrentContext {
    SliceMapT<OwnedValue>& map;
    std::array<std::atomic<size_t>, 7>& destructions;
    SliceMap& pin_target;
    std::barrier<> phase{2};
    std::array<std::atomic<size_t>, 3> visits{};
};
/** --------------------------------------------------------------------------------------------------------- Concurrent Writer
 * @brief Replaces, appends, and resets while the controlled callback protects its original payload.
 */
void concurrent_writer(ConcurrentContext* state) {
    state->phase.arrive_and_wait();
    state->map.emplace(10, 110, state->destructions[3]);
    state->map.emplace(20, 120, state->destructions[4]);
    state->map.emplace(40, 140, state->destructions[5]);
    state->map.reset();
    state->map.emplace(90, 190, state->destructions[6]);
    SliceMap::gc();
    state->phase.arrive_and_wait();
}
/** --------------------------------------------------------------------------------------------------------- Concurrent Visit
 * @brief Holds each callback payload across writer actions and reads the new map recursively.
 */
void concurrent_visit(int64_t identifier, Slice* slice, void* context) {
    auto& state = *static_cast<ConcurrentContext*>(context);
    require(identifier == 10 || identifier == 20 || identifier == 30,
        "concurrent traversal included a later append or switched reset generations");
    state.visits[static_cast<size_t>(identifier / 10 - 1)]
        .fetch_add(1, std::memory_order_relaxed);
    if (identifier == 10) {
        state.phase.arrive_and_wait();
        state.phase.arrive_and_wait();
        require(state.destructions[0].load(std::memory_order_relaxed) == 0
            && slice->get_as<OwnedValue>().value == 10,
            "concurrent replacement or reset finalized an active callback payload");
        require(state.map.size() == 1 && state.map.find(90) == 0,
            "callback reentry did not see the new generation");
        require(state.map.get_slice(90).get_as<OwnedValue>().value == 190
            && state.map.ids() == std::vector<int64_t>{90},
            "callback read crossed reset generations");
        std::atomic<size_t> nested_visits{0};
        state.map.for_each(&count_typed, &nested_visits);
        require(nested_visits.load() == 1, "recursive traversal inherited the outer generation");
        state.pin_target.find(-1);
        SliceMap::gc();
        require(state.destructions[0].load(std::memory_order_relaxed) == 0,
            "recursive reads cleared the outer payload's protection");
    }
    if (identifier == 20) {
        const int64_t value = slice->get_as<OwnedValue>().value;
        require(value == 20 || value == 120, "concurrent traversal selected another row's payload");
    }
    if (identifier == 30) {
        require(slice->get_as<OwnedValue>().value == 30,
            "traversal failed to retain an unvisited row across reset");
    }
}
/** --------------------------------------------------------------------------------------------------------- Concurrent Changes
 * @brief Checks bounded parallel traversal under replacement, append, reset, and collection.
 */
void concurrent_changes() {
    std::array<std::atomic<size_t>, 7> destructions{};
    SliceMapT<OwnedValue> map(3);
    for (size_t index = 0; index < 3; ++index) {
        const int64_t identifier = static_cast<int64_t>((index + 1) * 10);
        map.emplace(identifier, identifier, destructions[index]);
    }
    SliceMap pin_target(0);
    ConcurrentContext context{map, destructions, pin_target};
    std::thread writer(&concurrent_writer, &context);
    map.for_each(&concurrent_visit, &context);
    writer.join();
    for (const auto& visits : context.visits) {
        require(visits.load() == 1, "parallel traversal lost or repeated a captured row");
    }
    SliceMap::gc();
    for (size_t index = 0; index < 6; ++index) {
        require(destructions[index].load(std::memory_order_relaxed) == 1,
            "concurrent traversal leaked or double-finalized retired typed storage");
    }
    map.reset();
    map.find(-1);
    SliceMap::gc();
    require(destructions[6].load(std::memory_order_relaxed) == 1,
        "concurrent test retained its last map generation");
}
/** --------------------------------------------------------------------------------------------------------- Parallel Context
 * @brief Holds active callbacks until the caller proves dispatch overlaps and waits for completion.
 */
struct ParallelContext {
    static constexpr size_t count = 1025;
    SliceMap& map;
    std::array<std::atomic<size_t>, count> visits{};
    std::atomic<size_t> active{0};
    std::atomic<size_t> peak{0};
    std::atomic<bool> release{false};
    std::atomic<bool> finished{false};
};
/** --------------------------------------------------------------------------------------------------------- Parallel Visit
 * @brief Blocks an active callback until released while recording exact row coverage.
 */
void parallel_visit(int64_t identifier, Slice* slice, void* context) {
    auto& state = *static_cast<ParallelContext*>(context);
    require(identifier >= 0 && static_cast<size_t>(identifier) < ParallelContext::count,
        "parallel dispatch selected an invalid row");
    require(slice->get_as<int64_t>() == identifier, "parallel dispatch selected another payload");
    state.visits[static_cast<size_t>(identifier)].fetch_add(1, std::memory_order_relaxed);
    const size_t active = state.active.fetch_add(1, std::memory_order_relaxed) + 1;
    size_t peak = state.peak.load(std::memory_order_relaxed);
    while (peak < active && !state.peak.compare_exchange_weak(peak, active,
        std::memory_order_relaxed)) {}
    while (!state.release.load(std::memory_order_acquire)) {
        state.release.wait(false, std::memory_order_acquire);
    }
    state.active.fetch_sub(1, std::memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Parallel Dispatch
 * @brief Marks caller completion only after every callback returns from the public traversal.
 */
void parallel_dispatch(ParallelContext* context) {
    context->map.for_each(&parallel_visit, context);
    context->finished.store(true, std::memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Parallel Completion
 * @brief Verifies real callback overlap and synchronous completion across segment boundaries.
 */
void parallel_completion() {
    SliceMap map(1);
    for (size_t index = 0; index < ParallelContext::count; ++index) {
        map.add_slice(index, payload(static_cast<int64_t>(index)));
    }
    ParallelContext context{map};
    std::thread caller(&parallel_dispatch, &context);
#if defined(__APPLE__)
    const size_t workers = std::thread::hardware_concurrency();
#else
    const size_t workers = static_cast<size_t>(omp_get_max_threads());
#endif
    const size_t overlap = std::min(size_t(2), workers);
    require(overlap > 0, "parallel test could not probe the worker count");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (context.active.load(std::memory_order_relaxed) < overlap) {
        require(std::chrono::steady_clock::now() < deadline,
            "for_each did not schedule callbacks concurrently");
        std::this_thread::yield();
    }
    require(!context.finished.load(std::memory_order_acquire),
        "for_each returned before its callbacks completed");
    context.release.store(true, std::memory_order_release);
    context.release.notify_all();
    caller.join();
    require(context.finished.load(std::memory_order_acquire)
        && context.active.load() == 0 && context.peak.load() >= overlap,
        "parallel traversal failed to join its active callbacks");
    for (const auto& visits : context.visits) {
        require(visits.load() == 1, "parallel dispatch lost or duplicated a published row");
    }
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs public iteration contracts with deterministic callback and writer interleavings.
 */
int main() {
    LOG_INFO_STREAM << "Checking SliceMap iteration boundaries and identifier conversions";
    boundaries();
    LOG_INFO_STREAM << "Checking SliceMap hook-completed publication boundaries";
    publication_prefix();
    LOG_INFO_STREAM << "Checking SliceMap callback exceptions and recursive dispatch";
    exceptions();
    reentry(false);
    reentry(true);
    LOG_INFO_STREAM << "Checking SliceMap iteration during concurrent replacement, append, and reset";
    concurrent_changes();
    LOG_INFO_STREAM << "Checking SliceMap parallel callback overlap and completion";
    parallel_completion();
    LOG_INFO_STREAM << "SliceMap iteration contracts passed";
}
