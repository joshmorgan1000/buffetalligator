/** --------------------------------------------------------------------------------------------------------- Network Exit Test
 * @file network_exit_test.cpp
 * @brief Checks arena teardown with live callbacks under both network and allocator startup orders.
 */
#include <alligator.hpp>
#include <alligator/containers.hpp>
#include <memory/lifetime.hpp>
#include "functional_support.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <semaphore>
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
    require(slice.get_as<uint64_t>() == 42, "arena teardown invalidated an active callback Slice");
    slice.free();
    receive_finished.store(true, std::memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Cancelled Response
 * @brief Uses the arena during shutdown before recording one completed response cancellation.
 */
void cancelled_response(Slice slice) {
    require(!slice, "pending network response completed without cancellation");
    {
        Slice during_shutdown(size_t(64));
        during_shutdown.get_as<uint64_t>() = 77;
        require(during_shutdown.get_as<uint64_t>() == 77,
            "shutdown callback could not use the live arena");
    }
    cancellations.fetch_add(1, std::memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Release Receive
 * @brief Lets the held reactor callback finish while process-exit destructors start.
 */
void release_at_exit() { release_receive.release(); }
/** --------------------------------------------------------------------------------------------------------- Verify Exit
 * @brief Checks that arena destruction itself drained callbacks before older exit handlers run.
 */
void verify_exit(void*) {
    require(receive_finished.load(std::memory_order_acquire),
        "arena teardown completed before its active receive callback retired");
    require(cancellations.load(std::memory_order_acquire) == 1,
        "arena teardown completed before its pending response callback retired");
    ba_net_shutdown();
    LOG_INFO_STREAM << "Network callbacks retired before arena teardown";
}
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
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Uses fresh child processes to exercise genuine process-exit destructor registration.
 */
int main(int count, char** arguments) {
    if (count == 1) {
        for (const char* scenario : {"listen-first", "allocate-first", "callback-shutdown"}) {
            LOG_INFO_STREAM << "Checking network arena teardown: " << scenario;
            pid_t child;
            char* child_arguments[]{arguments[0], const_cast<char*>(scenario), nullptr};
            require(posix_spawn(&child, arguments[0], nullptr, nullptr, child_arguments, environ) == 0,
                "network exit child process could not start");
            int status = 0;
            require(waitpid(child, &status, 0) == child && WIFEXITED(status)
                && WEXITSTATUS(status) == 0, "network exit child process failed");
        }
        LOG_INFO_STREAM << "Network startup order and callback teardown contracts passed";
        return 0;
    }
    require(count == 2, "network exit test received unexpected arguments");
    const bool allocate_first = std::strcmp(arguments[1], "allocate-first") == 0;
    callback_shutdown = std::strcmp(arguments[1], "callback-shutdown") == 0;
    require(allocate_first || callback_shutdown || std::strcmp(arguments[1], "listen-first") == 0,
        "network exit test received an unknown scenario");
    LOG_INFO_STREAM << "Starting network exit scenario: " << arguments[1];
    std::optional<Slice> source;
    if (allocate_first) {
        static RuntimeFinalizer verification(nullptr, &verify_exit);
        source.emplace(size_t(64));
    }
    const uint16_t port = listener_port();
    SliceChannel::listen(port, SliceChannel::Protocol::UDP, receive);
    if (!allocate_first) {
        static RuntimeFinalizer verification(nullptr, &verify_exit);
        source.emplace(size_t(64));
    }
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
}
