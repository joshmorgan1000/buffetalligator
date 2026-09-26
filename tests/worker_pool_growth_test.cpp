/** --------------------------------------------------------------------------------------------------------- Worker Pool Growth Test
 * @file worker_pool_growth_test.cpp
 * @brief Verifies the worker pool grows under a queue backlog and keeps growing after stale spawn
 * requests: a burst of blocking tasks must spread across many workers, twice in a row.
 */
#include <alligator.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <future>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using buffetalligator::Alligator;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
/// @brief Wall time each task holds its worker.
constexpr std::chrono::milliseconds TASK_HOLD{100};
/** --------------------------------------------------------------------------------------------------------- Burst
 * @brief Submits `count` tasks that each hold a worker for `TASK_HOLD`, waits for all of them, and
 * reports the distinct workers they ran on and the wall time of the whole burst.
 * @param count Tasks in the burst.
 * @param workers Receives the number of distinct worker threads that ran the tasks.
 * @return The burst's wall time.
 */
std::chrono::milliseconds burst(size_t count, size_t& workers) {
    std::mutex seen_mutex;
    std::set<std::thread::id> seen;
    std::vector<std::future<void>> futures;
    futures.reserve(count);
    const auto start = std::chrono::steady_clock::now();
    for (size_t index = 0; index < count; ++index) {
        futures.push_back(Alligator::inst().submit([&seen_mutex, &seen]() {
            {
                std::lock_guard<std::mutex> lock(seen_mutex);
                seen.insert(std::this_thread::get_id());
            }
            const auto deadline = std::chrono::steady_clock::now() + TASK_HOLD;
            while (std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
        }));
    }
    for (std::future<void>& future : futures) future.get();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    std::lock_guard<std::mutex> lock(seen_mutex);
    workers = seen.size();
    return elapsed;
}
} // namespace

int main() {
    Alligator& alligator = Alligator::inst();
    const size_t cores = std::max<size_t>(4u, std::thread::hardware_concurrency());
    const size_t count = std::min<size_t>(32u, cores * 2u);
    /// The pool grows while the backlog exceeds twice the active workers, so a burst of `count` tasks
    /// must reach well past the two workers a dead thread changer leaves behind.
    const size_t expected_workers = std::min<size_t>(alligator.max_threads(), std::max<size_t>(4u, count / 4u));
    /// Two workers would need count / 2 holds back to back; a grown pool finishes in a fraction of that.
    const std::chrono::milliseconds ceiling = TASK_HOLD * static_cast<int>((count + expected_workers - 1) / expected_workers) * 2;
    for (int round = 1; round <= 2; ++round) {
        size_t workers = 0;
        const std::chrono::milliseconds elapsed = burst(count, workers);
        std::printf("burst %d: %zu tasks over %zu workers in %lld ms (expected >= %zu workers, <= %lld ms)\n",
            round, count, workers, static_cast<long long>(elapsed.count()), expected_workers, static_cast<long long>(ceiling.count()));
        require(workers >= expected_workers, "worker pool did not grow under a queue backlog");
        require(elapsed <= ceiling, "burst ran as if on a pool that stopped growing");
        /// Idle long enough for every worker to have issued, and the changer to have consumed, stale requests.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::printf("worker pool growth ok\n");
    return 0;
}
