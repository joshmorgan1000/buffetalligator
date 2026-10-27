/** --------------------------------------------------------------------------------------------------------- Network Exit Test
 * @file network_exit_test.cpp
 * @brief Checks process teardown with live network callbacks under both network and allocator startup orders,
 * observed from a parent process through a pipe.
 */
#include <alligator.hpp>
#include <alligator/containers.hpp>
#include "functional_support.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <semaphore>
#include <string>
#include <thread>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
extern "C" {
#include "network/ba_network.h"
}
extern char** environ;

using namespace buffetalligator;
using functional::require;
namespace {
std::binary_semaphore received{0};
std::binary_semaphore release_receive{0};
std::atomic<bool> receive_finished{false};
std::atomic<size_t> cancellations{0};
bool callback_shutdown = false;
/// @brief The pipe the child reports each retired callback through.
int evidence = -1;
/** --------------------------------------------------------------------------------------------------------- Report
 * @brief Writes one callback mark to the parent.
 */
void report(char mark) {
    if (write(evidence, &mark, 1) != 1) std::abort();
}
/** --------------------------------------------------------------------------------------------------------- Port
 * @brief Reserves an operating-system-selected loopback port for the UDP listener.
 */
uint16_t listener_port() {
    const int descriptor = socket(AF_INET, SOCK_DGRAM, 0);
    require(descriptor >= 0, "network exit socket creation failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
        "network exit socket bind failed");
    socklen_t length = sizeof(address);
    require(getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &length) == 0,
        "network exit socket name failed");
    require(close(descriptor) == 0, "network exit socket close failed");
    return ntohs(address.sin_port);
}
/** --------------------------------------------------------------------------------------------------------- Receive
 * @brief Retains a live callback Slice until process exit or explicit concurrent shutdown starts.
 */
void receive(Slice slice) {
    require(slice && slice.size_bytes() == 64 && slice.get_as<uint64_t>() == 42,
        "network exit callback received an incorrect payload");
    if (callback_shutdown) ba_net_shutdown();
    received.release();
    release_receive.acquire();
    require(slice.get_as<uint64_t>() == 42, "teardown invalidated an active callback Slice");
    slice.free();
    receive_finished.store(true, std::memory_order_release);
    report('R');
}
/** --------------------------------------------------------------------------------------------------------- Cancelled Response
 * @brief Uses the Alligator during shutdown before recording one completed response cancellation.
 */
void cancelled_response(Slice slice) {
    require(!slice, "pending network response completed without cancellation");
    {
        Slice during_shutdown(size_t(64));
        during_shutdown.get_as<uint64_t>() = 77;
        require(during_shutdown.get_as<uint64_t>() == 77, "shutdown callback could not use the live Alligator");
    }
    cancellations.fetch_add(1, std::memory_order_release);
    report('C');
}
/** --------------------------------------------------------------------------------------------------------- Release Receive
 * @brief Lets the held reactor callback finish while process-exit handlers start.
 */
void release_at_exit() { release_receive.release(); }
/** --------------------------------------------------------------------------------------------------------- Concurrent Shutdown
 * @brief Requires every concurrent shutdown caller to observe the completed callback drain.
 */
void concurrent_shutdown(std::barrier<>* gate) {
    gate->arrive_and_wait();
    ba_net_shutdown();
    require(receive_finished.load(std::memory_order_acquire)
        && cancellations.load(std::memory_order_acquire) == 1,
        "concurrent shutdown returned before reactor callbacks retired");
}
/** --------------------------------------------------------------------------------------------------------- Observe Scenario
 * @brief Runs one scenario in a child process and requires both callbacks to retire before it exits.
 */
void observe_scenario(char* program, const char* scenario) {
    LOG_INFO_STREAM << "Checking network teardown: " << scenario;
    int pipe_ends[2];
    require(pipe(pipe_ends) == 0, "network exit test could not create its pipe");
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, pipe_ends[0]);
    char descriptor[16];
    std::snprintf(descriptor, sizeof(descriptor), "%d", pipe_ends[1]);
    char* child_arguments[]{program, const_cast<char*>(scenario), descriptor, nullptr};
    pid_t child;
    require(posix_spawn(&child, program, &actions, nullptr, child_arguments, environ) == 0,
        "network exit child process could not start");
    posix_spawn_file_actions_destroy(&actions);
    close(pipe_ends[1]);
    size_t receives = 0, cancels = 0;
    char marks[64];
    for (ssize_t got = read(pipe_ends[0], marks, sizeof(marks)); got > 0; got = read(pipe_ends[0], marks, sizeof(marks))) {
        for (ssize_t index = 0; index < got; ++index) {
            receives += marks[index] == 'R';
            cancels += marks[index] == 'C';
        }
    }
    close(pipe_ends[0]);
    int status = 0;
    require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "network exit child process failed");
    require(receives == 1, "teardown completed before its active receive callback retired");
    require(cancels == 1, "teardown completed before its pending response callback was cancelled");
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Uses fresh child processes to exercise genuine process-exit teardown with live callbacks.
 */
int main(int count, char** arguments) {
    if (count == 1) {
        for (const char* scenario : {"listen-first", "allocate-first", "callback-shutdown"}) {
            observe_scenario(arguments[0], scenario);
        }
        LOG_INFO_STREAM << "Network startup order and callback teardown contracts passed";
        return 0;
    }
    require(count == 3, "network exit test received unexpected arguments");
    require(setenv("ALLIGATOR_GPU_BACKEND", "cpu", 1) == 0, "CPU selection failed");
    evidence = std::atoi(arguments[2]);
    const bool allocate_first = std::strcmp(arguments[1], "allocate-first") == 0;
    callback_shutdown = std::strcmp(arguments[1], "callback-shutdown") == 0;
    require(allocate_first || callback_shutdown || std::strcmp(arguments[1], "listen-first") == 0,
        "network exit test received an unknown scenario");
    LOG_INFO_STREAM << "Starting network exit scenario: " << arguments[1];
    std::optional<Slice> source;
    if (allocate_first) source.emplace(size_t(64));
    const uint16_t port = listener_port();
    SliceChannel::listen(port, SliceChannel::Protocol::UDP, receive);
    if (!allocate_first) source.emplace(size_t(64));
    source->get_as<uint64_t>() = 42;
    SliceChannel::send(*source, "127.0.0.1", port, SliceChannel::Protocol::UDP, cancelled_response);
    require(received.try_acquire_for(std::chrono::seconds(5)),
        "network exit receive callback did not reach its controlled hold");
    if (callback_shutdown) {
        std::barrier gate(17);
        std::array<std::thread, 16> callers;
        for (auto& caller : callers) caller = std::thread(concurrent_shutdown, &gate);
        gate.arrive_and_wait();
        release_receive.release();
        for (auto& caller : callers) caller.join();
    } else {
        require(std::atexit(release_at_exit) == 0, "network exit callback release registration failed");
        LOG_INFO_STREAM << "Exiting with an active receive and pending response callback";
    }
    return 0;
}
