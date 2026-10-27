/** --------------------------------------------------------------------------------------------------------- Kitchen Shutdown Test
 * @file kitchen_shutdown_test.cpp
 * @brief Checks process teardown completes every accepted Order and completion callback left behind occupied
 * workers, observed from a parent process through a pipe.
 */
#include <alligator/kitchen.hpp>
#include "functional_support.hpp"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <latch>
#include <thread>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;

using functional::require;
namespace {
constexpr size_t pending_count = 4096;
/// @brief The pipe the child reports each completion through.
int evidence = -1;
/** --------------------------------------------------------------------------------------------------------- Shutdown State
 * @brief Retains the worker gates through the real runtime teardown.
 */
struct ShutdownState {
    std::latch release_workers{1};
    std::latch workers_entered{static_cast<std::ptrdiff_t>(std::thread::hardware_concurrency())};
};
ShutdownState* pending = nullptr;
/** --------------------------------------------------------------------------------------------------------- Report
 * @brief Writes one completion mark to the parent.
 */
void report(char mark) {
    if (write(evidence, &mark, 1) != 1) std::abort();
}
/** --------------------------------------------------------------------------------------------------------- Release Workers
 * @brief Opens the worker gate when main returns, so teardown itself must drain the queued backlog.
 */
struct ReleaseWorkers {
    ~ReleaseWorkers() { pending->release_workers.count_down(); }
};
/** --------------------------------------------------------------------------------------------------------- Hold Worker
 * @brief Occupies one worker until the program begins static teardown.
 */
void hold_worker(void* context) {
    auto* state = static_cast<ShutdownState*>(context);
    state->workers_entered.count_down();
    state->release_workers.wait();
}
/// @brief Records execution of one accepted queued Order.
void complete_order(void*) { report('o'); }
/// @brief Records completion of one accepted queued Order.
void complete_callback(void*) { report('c'); }
/** --------------------------------------------------------------------------------------------------------- Observe Child
 * @brief Spawns the backlog scenario in a child process and counts the marks it reports before exiting.
 */
void observe_child(char* program) {
    int pipe_ends[2];
    require(pipe(pipe_ends) == 0, "kitchen shutdown test could not create its pipe");
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, pipe_ends[0]);
    char descriptor[16];
    std::snprintf(descriptor, sizeof(descriptor), "%d", pipe_ends[1]);
    char* child_arguments[]{program, descriptor, nullptr};
    pid_t child;
    require(posix_spawn(&child, program, &actions, nullptr, child_arguments, environ) == 0,
        "kitchen shutdown child process could not start");
    posix_spawn_file_actions_destroy(&actions);
    close(pipe_ends[1]);
    size_t orders = 0, callbacks = 0;
    char marks[4096];
    for (ssize_t got = read(pipe_ends[0], marks, sizeof(marks)); got > 0; got = read(pipe_ends[0], marks, sizeof(marks))) {
        for (ssize_t index = 0; index < got; ++index) {
            orders += marks[index] == 'o';
            callbacks += marks[index] == 'c';
        }
    }
    close(pipe_ends[0]);
    int status = 0;
    require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "kitchen shutdown child process failed");
    require(orders == pending_count, "Kitchen shutdown discarded an accepted Order: ran ", orders);
    require(callbacks == pending_count, "Kitchen shutdown discarded an accepted completion callback: ran ", callbacks);
    LOG_INFO_STREAM << "Kitchen shutdown completed all accepted Orders and callbacks";
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief As the parent, observes the child; as the child, leaves a queued backlog behind occupied workers.
 */
int main(int count, char** arguments) {
    if (count == 1) {
        LOG_INFO_STREAM << "Checking shutdown drains accepted Orders and completion callbacks";
        observe_child(arguments[0]);
        return 0;
    }
    require(count == 2, "kitchen shutdown test received unexpected arguments");
    evidence = std::atoi(arguments[1]);
    pending = new ShutdownState;
    static ReleaseWorkers release_on_exit;
    for (size_t worker = 0; worker < std::thread::hardware_concurrency(); ++worker) {
        buffetalligator::Kitchen::submit(&hold_worker, static_cast<void*>(pending));
    }
    pending->workers_entered.wait();
    for (size_t index = 0; index < pending_count; ++index) {
        buffetalligator::Kitchen::submit(&complete_order, static_cast<void*>(pending), &complete_callback,
            static_cast<void*>(pending));
    }
    LOG_INFO_STREAM << "Leaving a queued backlog behind occupied workers for teardown to complete";
    return 0;
}
