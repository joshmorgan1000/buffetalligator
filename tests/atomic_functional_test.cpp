/** --------------------------------------------------------------------------------------------------------- Atomic Functional Test
 * @file atomic_functional_test.cpp
 * @brief Exercises type-erased atomic operations, ownership, registry lookup, and thread handoffs.
 */
#include <alligator.hpp>
#include "functional_support.hpp"
#include <barrier>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace buffetalligator;
using functional::require;
using functional::require_throws;
/** --------------------------------------------------------------------------------------------------------- Native Atomics
 * @brief Checks arithmetic, bit operations, comparisons, type checks, and serialization.
 */
static void native_atomics() {
    int initial = 12;
    AtomicContainer number(initial);
    require(number.is_type<int>() && number.is_native_atomic(), "native type was not retained");
    require(number.fetch_add(5) == 12 && number.fetch_sub(2) == 17, "arithmetic prior values differ");
    require(number.fetch_and(7) == 15 && number.fetch_or(8) == 7 && number.fetch_xor(3) == 15,
        "bit operations returned incorrect prior values");
    int replacement = 24;
    number.store(replacement);
    require(number.exchange(initial) == 24 && number.load<int>() == 12,
        "lvalue store or exchange failed");
    int expected = 0;
    require(!number.compare_exchange_strong(expected, 20,
        std::memory_order_acq_rel, std::memory_order_acquire) && expected == 12,
        "failed comparison did not update expected");
    require(number.compare_exchange_strong(expected, 20,
        std::memory_order_acq_rel, std::memory_order_acquire), "matching comparison failed");
    expected = 20;
    while (!number.compare_exchange_weak(expected, 31,
        std::memory_order_acq_rel, std::memory_order_acquire)) expected = 20;
    require(number.to_json().find("31") != std::string::npos, "JSON omitted the stored value");
    require_throws([&] { number.load<double>(); }, "wrong load type accepted");
    require_throws([&] { number.store(std::string("wrong")); }, "wrong store type accepted");
    AtomicContainer real(1.5);
    require(real.fetch_add(2.0) == 1.5 && real.load<double>() == 3.5,
        "floating point arithmetic failed");
    AtomicContainer flag(false);
    flag.store(true);
    require(flag.load<bool>(), "boolean store failed");
}
/** --------------------------------------------------------------------------------------------------------- Append
 * @brief Appends one suffix through the public fetch-modify callback contract.
 */
static std::string append(std::string&& value, char suffix) {
    value += suffix;
    return std::move(value);
}
/** --------------------------------------------------------------------------------------------------------- Complex Atomics
 * @brief Checks borrowing, copying, exchanges, and comparisons of non-native values.
 */
static void complex_atomics() {
    std::string initial = "first";
    AtomicContainer text(initial);
    require(!text.is_native_atomic() && text.load<std::string>() == initial,
        "complex lvalue construction failed");
    auto* borrowed = text.borrow<std::string>();
    *borrowed = "borrowed";
    text.return_borrowed(borrowed);
    require(text.load<std::string>() == "borrowed", "borrowed mutation was lost");
    text.store(initial);
    require(initial == "first", "storing an lvalue moved from the caller");
    require(text.exchange(std::string("second")) == "first", "complex exchange lost old value");
    std::string expected = "missing";
    require(!text.compare_exchange_strong(expected, std::string("third"),
        std::memory_order_acq_rel, std::memory_order_acquire) && expected == "second",
        "complex mismatch did not update expected");
    require(text.compare_exchange_weak(expected, std::string("third"),
        std::memory_order_acq_rel, std::memory_order_acquire), "complex comparison failed");
    require(text.fetch_modify<std::string, char>(append, std::memory_order_acq_rel, '!') == "third",
        "complex fetch-modify lost old value");
    require(text.load<std::string>() == "third!", "complex fetch-modify lost new value");
}
/** --------------------------------------------------------------------------------------------------------- Ownership
 * @brief Checks unique ownership of objects and non-owning atomic pointer values.
 */
static void ownership() {
    auto object = std::make_shared<int>(42);
    std::weak_ptr<int> lifetime = object;
    {
        AtomicContainer owned(std::make_unique<std::shared_ptr<int>>(std::move(object)));
        require(!object && !lifetime.expired(), "unique ownership transfer failed");
        require(**owned.ptr<std::shared_ptr<int>>() == 42, "owned object changed");
    }
    require(lifetime.expired(), "container did not destroy its owned object");
    int first = 3, second = 5;
    int* pointer = &first;
    AtomicContainer borrowed(pointer);
    require(borrowed.is_pointer() && borrowed.load<int*>() == &first, "pointer identity lost");
    require(borrowed.exchange(&second) == &first, "pointer exchange lost old pointer");
    pointer = &first;
    borrowed.store(pointer);
    require(borrowed.load<int*>() == &first && second == 5, "pointer store touched its pointee");
    auto native = std::make_unique<uint64_t>(91);
    AtomicContainer native_owner(std::move(native));
    require(!native && native_owner.load<uint64_t>() == 91, "native unique ownership failed");
}
/** --------------------------------------------------------------------------------------------------------- Throwing Modify
 * @brief Throws without modifying the stored value to exercise borrow cleanup.
 */
static std::string throwing_modify(std::string&&) {
    throw std::runtime_error("intentional callback failure");
}
/** --------------------------------------------------------------------------------------------------------- Exceptions
 * @brief Checks that a throwing callback does not leave the container permanently borrowed.
 */
static void exceptions() {
    AtomicContainer text(std::string("retained"));
    require_throws([&] {
        text.fetch_modify<std::string>(throwing_modify, std::memory_order_acq_rel);
    }, "callback exception was swallowed");
    require(text.load<std::string>() == "retained", "callback failure poisoned the container");
    text.store(std::string("recovered"));
    require(text.load<std::string>() == "recovered", "store after callback failure failed");
}
/** --------------------------------------------------------------------------------------------------------- Increment
 * @brief Increments a native value through the public modification callback.
 */
static uint64_t increment(uint64_t&& value) { return value + 1; }
/** --------------------------------------------------------------------------------------------------------- Concurrent Atomics
 * @brief Checks exact native updates and serialized complex reads and modifications.
 */
static void concurrent_atomics() {
    constexpr size_t rounds = 200;
    const size_t workers = std::clamp<size_t>(2 * std::thread::hardware_concurrency(), 16, 64);
    AtomicContainer number(uint64_t(0));
    AtomicContainer text{std::string()};
    std::barrier start(static_cast<std::ptrdiff_t>(workers));
    std::vector<std::thread> team;
    for (size_t worker = 0; worker < workers; ++worker) {
        team.emplace_back([&, worker] {
            start.arrive_and_wait();
            for (size_t round = 0; round < rounds; ++round) {
                if (worker % 2 == 0) number.fetch_add<uint64_t>(1, std::memory_order_relaxed);
                else number.fetch_modify<uint64_t>(increment, std::memory_order_relaxed);
                text.fetch_modify<std::string, char>(append, std::memory_order_acq_rel, 'x');
                const std::string snapshot = text.load<std::string>();
                require(snapshot.find_first_not_of('x') == std::string::npos,
                    "concurrent complex load observed corrupt storage");
            }
        });
    }
    for (auto& worker : team) worker.join();
    require(number.load<uint64_t>() == workers * rounds, "concurrent native update was lost");
    require(text.load<std::string>().size() == workers * rounds, "complex update was lost");
}
/** --------------------------------------------------------------------------------------------------------- Wait Notify
 * @brief Checks native wait and notification publish preceding non-atomic writes.
 */
static void wait_notify() {
    AtomicContainer ready(false);
    int payload = 0;
    std::thread reader([&] {
        ready.wait(false, std::memory_order_acquire);
        require(payload == 73, "wait did not acquire the published payload");
    });
    payload = 73;
    ready.store(true, std::memory_order_release);
    ready.notify_one();
    reader.join();
}
/** --------------------------------------------------------------------------------------------------------- Registry
 * @brief Checks key selection, duplicate rejection, moves, and ownership removal.
 */
static void registry() {
    AtomicRegistry values;
    require(values.empty() && values.get("missing") == nullptr, "new registry is not empty");
    int initial = 8;
    auto* number = values.create("number", initial);
    values.get_or_create("text", std::make_unique<std::string>("hello"));
    require(values.size() == 2 && values.contains("text"), "registry creation failed");
    require(values.get_or_create("number", 99) == number && number->load<int>() == 8,
        "get-or-create replaced an existing value");
    require_throws([&] { values.create("number", 19); }, "duplicate key accepted");
    require_throws([&] { values["absent"]; }, "missing subscript accepted");
    std::vector<std::string> selection{"text", "missing", "number", "text"};
    require(values.select(selection).size() == 2 && values.select("number").at("number") == number,
        "registry selection differs");
    require(values.keys().size() == 2 && values.list().size() == 2, "registry listing differs");
    const auto& constant = values;
    require(constant["number"] == number, "const registry access differs");
    AtomicRegistry moved(std::move(values));
    auto removed = moved.remove("number");
    require(removed.get() == number && !moved.contains("number"), "remove lost ownership");
    require(!moved.remove("missing"), "missing remove returned a value");
    moved.clear();
    require(moved.empty() && removed->load<int>() == 8, "clear destroyed a removed value");
}
/** --------------------------------------------------------------------------------------------------------- Global Registry
 * @brief Checks global lookup, selection, removal, and clearing through the static API.
 */
static void global_registry() {
    AtomicRegistry::global_clear();
    auto* first = AtomicRegistry::create_global("first", 11);
    auto* second = AtomicRegistry::get_or_create_global("second", std::make_unique<int>(22));
    require(AtomicRegistry::get_global("first") == first &&
        AtomicRegistry::get_or_create_global("first", 33) == first, "global identity changed");
    require(AtomicRegistry::global_size() == 2 && AtomicRegistry::global_contains("second"),
        "global creation failed");
    std::vector<std::string> keys{"second", "missing"};
    require(AtomicRegistry::select_global(keys).at("second") == second &&
        AtomicRegistry::select_global("first").size() == 1, "global selection failed");
    require(AtomicRegistry::global_keys().size() == 2 && AtomicRegistry::list_global().size() == 2,
        "global listing differs");
    auto removed = AtomicRegistry::remove_global("first");
    require(removed.get() == first && !AtomicRegistry::get_global("first"), "global remove failed");
    AtomicRegistry::global_clear();
    require(AtomicRegistry::global_empty() && second != nullptr, "global clear failed");
}
/** --------------------------------------------------------------------------------------------------------- Concurrent Registry
 * @brief Checks contending get-or-create calls share one container without losing increments.
 */
static void concurrent_registry() {
    constexpr size_t workers = 12, rounds = 200;
    AtomicRegistry registry;
    std::array<AtomicContainer*, workers> observed{};
    std::barrier start(static_cast<std::ptrdiff_t>(workers));
    std::vector<std::thread> team;
    for (size_t worker = 0; worker < workers; ++worker) {
        team.emplace_back([&, worker] {
            start.arrive_and_wait();
            observed[worker] = registry.get_or_create("shared", uint64_t(0));
            for (size_t round = 0; round < rounds; ++round) {
                registry.get("shared")->fetch_add<uint64_t>(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& worker : team) worker.join();
    for (auto* value : observed) require(value == observed[0], "get-or-create split identity");
    require(registry.size() == 1 && observed[0]->load<uint64_t>() == workers * rounds,
        "concurrent registry update was lost");
}
/** --------------------------------------------------------------------------------------------------------- Throwing Copy
 * @brief Provides a value whose copy construction fails inside registry insertion.
 */
struct ThrowingCopy {
    ThrowingCopy() = default;
    ThrowingCopy(ThrowingCopy&&) = default;
    ThrowingCopy(const ThrowingCopy&) { throw std::runtime_error("intentional copy failure"); }
};
/** --------------------------------------------------------------------------------------------------------- Registry Exceptions
 * @brief Checks failed value construction releases both local and global registry locks.
 */
static void registry_exceptions() {
    ThrowingCopy initial;
    AtomicRegistry registry;
    require_throws([&] { registry.create("failed", initial); }, "failed copy was accepted");
    require(registry.empty(), "failed construction inserted a value");
    require(registry.create("recovered", 42)->load<int>() == 42,
        "failed construction poisoned the registry lock");
    require_throws([&] { AtomicRegistry::get_or_create_global("failed", initial); },
        "failed global copy was accepted");
    require(AtomicRegistry::global_empty(), "failed global construction inserted a value");
    AtomicRegistry::create_global("recovered", 73);
    require(AtomicRegistry::get_global("recovered")->load<int>() == 73,
        "failed construction poisoned the global lock");
    AtomicRegistry::global_clear();
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Dispatches independent container and registry functional cases.
 */
int main(int count, char** arguments) {
    return functional::run(count, arguments, {
        {"native_atomics", native_atomics}, {"complex_atomics", complex_atomics},
        {"atomic_ownership", ownership}, {"atomic_exceptions", exceptions},
        {"concurrent_atomics", concurrent_atomics}, {"atomic_wait_notify", wait_notify},
        {"registry", registry}, {"global_registry", global_registry},
        {"concurrent_registry", concurrent_registry}, {"registry_exceptions", registry_exceptions}
    });
}
