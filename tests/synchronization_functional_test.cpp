/** --------------------------------------------------------------------------------------------------------- Synchronization Functional Test
 * @file synchronization_functional_test.cpp
 * @brief Exercises shared exclusion through the AtomicMutex with overlapping readers and blocked writers.
 */
#include <alligator.hpp>
#include <alligator/atomics.hpp>
#include "functional_support.hpp"
#include <barrier>
#include <chrono>
#include <mutex>
#include <semaphore>
#include <shared_mutex>
#include <thread>
#include <vector>

using namespace buffetalligator;
using functional::require;
using namespace std::chrono_literals;
/** --------------------------------------------------------------------------------------------------------- Mutex
 * @brief Checks concurrent shared access and exclusion of writers while readers hold the lock.
 */
static void mutex() {
    AtomicMutex gate;
    std::binary_semaphore reader_entered(0), release_reader(0), writer_entered(0);
    int value = 0;
    gate.lock_shared();
    std::thread reader([&] {
        std::shared_lock lock(gate);
        reader_entered.release();
        release_reader.acquire();
        require(value == 0, "writer entered while a reader held the mutex");
    });
    require(reader_entered.try_acquire_for(2s), "shared readers could not overlap");
    std::thread writer([&] {
        std::lock_guard lock(gate);
        value = 1;
        writer_entered.release();
    });
    require(!writer_entered.try_acquire_for(30ms), "writer bypassed active readers");
    release_reader.release();
    reader.join();
    gate.unlock_shared();
    require(writer_entered.try_acquire_for(2s), "writer did not acquire after readers left");
    writer.join();
    require(value == 1 && !gate.is_locked(), "mutex did not return to its unlocked state");
    constexpr size_t workers = 8, rounds = 1000;
    std::barrier start(static_cast<std::ptrdiff_t>(workers));
    std::vector<std::thread> team;
    for (size_t worker = 0; worker < workers; ++worker) {
        team.emplace_back([&] {
            start.arrive_and_wait();
            for (size_t round = 0; round < rounds; ++round) {
                std::lock_guard lock(gate);
                ++value;
            }
        });
    }
    for (auto& worker : team) worker.join();
    require(value == 1 + workers * rounds, "mutex lost protected updates");
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Dispatches independent synchronization functional cases with CTest time limits.
 */
int main(int count, char** arguments) {
    return functional::run(count, arguments, {{"mutex", mutex}});
}
