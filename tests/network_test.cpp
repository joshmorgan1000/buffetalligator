/** --------------------------------------------------------------------------------------------------------- Network Contract Tests
 * @file network_test.cpp
 * @brief Verifies public Slice channels across processes and rejects malformed or forged private frames.
 */
#include "test_support.hpp"
#include <alligator.hpp>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
extern "C" {
#include "network/ba_network.h"
}

using namespace buffetalligator;
namespace {
using Protocol = SliceChannel::Protocol;
uint16_t port;
Protocol protocol;
std::counting_semaphore<4096> completed{0};
std::mutex results_mutex;
std::vector<Slice> results;
/** --------------------------------------------------------------------------------------------------------- Allocate Test Placement
 * @brief Supplies genuine registered storage whose type differs between the two processes.
 */
Placemat::Handle* allocate(size_t bytes, void*) {
    void* pointer = nullptr;
    if (posix_memalign(&pointer, 64, bytes)) return nullptr;
    std::memset(pointer, 0, bytes);
    return new Placemat::Handle{pointer, nullptr};
}
/** --------------------------------------------------------------------------------------------------------- Release Test Placement
 * @brief Releases the placement's allocation through its actual callback.
 */
void deallocate(Placemat::Handle* handle, void*) { std::free(handle->substrate_handle); }
/** --------------------------------------------------------------------------------------------------------- Host Test Placement
 * @brief Returns the registered allocation's host address.
 */
void* host(Placemat::Handle* handle) { return handle->substrate_handle; }
/** --------------------------------------------------------------------------------------------------------- Register
 * @brief Registers one named placement for cross-process identifier tests.
 */
void register_placement(const char* name) {
    PlacementDescription description;
    description.name = name;
    description.slab_bytes = 16u * 1024u * 1024u;
    description.budget_bytes = 256u * 1024u * 1024u;
    description.allocate = allocate;
    description.deallocate = deallocate;
    description.get_host_ptr = host;
    BuffetMenu::register_type(description);
}
/** --------------------------------------------------------------------------------------------------------- Response
 * @brief Retains callback results for validation by the test thread.
 */
void response(Slice slice) {
    {
        std::lock_guard lock(results_mutex);
        results.push_back(std::move(slice));
    }
    completed.release();
}
/** --------------------------------------------------------------------------------------------------------- Receive
 * @brief Mutates the received Slice and replies using only the three public channel operations.
 */
void receive(Slice slice) {
    slice.data<unsigned char>()[slice.size_bytes() - 1] ^= 0xA5;
    SliceChannel::send(std::move(slice), "", port, protocol);
}
/** --------------------------------------------------------------------------------------------------------- One-way Receiver
 * @brief Confirms a receiver can use its normal reply path when the sender discards responses.
 */
void receive_oneway(Slice slice) {
    receive(std::move(slice));
    completed.release();
}
/** --------------------------------------------------------------------------------------------------------- Idle Receiver
 * @brief Accepts a request without producing a response.
 */
void idle(Slice) {}
/** --------------------------------------------------------------------------------------------------------- Port
 * @brief Reserves an available loopback service number for a short-lived test server.
 */
uint16_t unused_port() {
    int descriptor = socket(AF_INET, SOCK_STREAM, 0);
    TEST_REQUIRE(descriptor >= 0, "test socket creation failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_EQUAL(bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0,
            "test bind failed");
    socklen_t length = sizeof(address);
    TEST_EQUAL(getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &length), 0,
            "test name failed");
    ::close(descriptor);
    return ntohs(address.sin_port);
}
/** --------------------------------------------------------------------------------------------------------- Child
 * @brief Keeps a separate server process alive until the test scope finishes.
 */
struct Child {
    pid_t process;
    int control;
    Child(const char* executable, Protocol selected, uint16_t number) {
        int ready[2], commands[2];
        TEST_REQUIRE(pipe(ready) == 0 && pipe(commands) == 0, "test pipes failed");
        std::string protocol_text = std::to_string(static_cast<unsigned>(selected));
        std::string port_text = std::to_string(number);
        std::string ready_text = std::to_string(ready[1]);
        std::string command_text = std::to_string(commands[0]);
        process = fork();
        TEST_REQUIRE(process >= 0, "test fork failed");
        if (!process) {
            ::close(ready[0]);
            ::close(commands[1]);
            execl(executable, executable, "server", protocol_text.c_str(), port_text.c_str(),
                  ready_text.c_str(), command_text.c_str(), static_cast<char*>(nullptr));
            _exit(2);
        }
        ::close(ready[1]);
        ::close(commands[0]);
        control = commands[1];
        char message = 0;
        ssize_t bytes = read(ready[0], &message, 1);
        ::close(ready[0]);
        if (bytes != 1 || message != 'R') {
            ::close(control);
            waitpid(process, nullptr, 0);
            throw std::runtime_error("child listener failed");
        }
    }
    ~Child() {
        ::close(control);
        int status;
        waitpid(process, &status, 0);
    }
};
/** --------------------------------------------------------------------------------------------------------- Round Trips
 * @brief Checks multi-threaded payload, subview, novel backing, and placement preservation.
 */
void round_trips(const char* executable, Protocol selected) {
    uint16_t number = unused_port();
    Child server(executable, selected, number);
    std::printf("Testing protocol %u across processes on port %u\n",
                static_cast<unsigned>(selected), number);
    const bool datagram = (static_cast<unsigned>(selected) & 127) == 1;
    const size_t sizes[] = {1, 8193, datagram ? 48001u : 2097155u};
    for (size_t bytes : sizes) {
        Slice backing(bytes + 13, true, BuffetMenu::get("NetworkTest"));
        Slice view = backing.slice(7, bytes);
        for (size_t index = 0; index < bytes; ++index)
            view.data<unsigned char>()[index] = index % 251;
        SliceChannel::send(std::move(view), "127.0.0.1", number, selected, response);
        backing.free();
        TEST_REQUIRE(completed.try_acquire_for(std::chrono::seconds(15)), "response did not arrive");
        Slice result;
        {
            std::lock_guard lock(results_mutex);
            result = std::move(results.back());
            results.pop_back();
        }
        TEST_REQUIRE(result && result.size_bytes() == bytes, "response length changed");
        TEST_REQUIRE(result.is_novel(), "novel backing was lost");
        TEST_EQUAL(std::strcmp(result.placement()->name(), "NetworkTest"), 0,
                "placement was not preserved by name");
        for (size_t index = 0; index < bytes; ++index) {
            unsigned expected = index % 251;
            if (index + 1 == bytes) expected ^= 0xA5;
            TEST_EQUAL(result.data<unsigned char>()[index], expected,
                    "payload changed across the wire");
        }
    }
    std::vector<std::thread> workers;
    for (unsigned index = 0; index < 8; ++index)
        workers.emplace_back([=] {
            for (unsigned message = 0; message < 4; ++message) {
                Slice slice(1024);
                std::memset(slice.raw(), index + message + 1, slice.size_bytes());
                SliceChannel::send(std::move(slice), "localhost", number, selected, response);
            }
        });
    for (auto& worker : workers) worker.join();
    for (unsigned index = 0; index < 32; ++index)
        TEST_REQUIRE(completed.try_acquire_for(std::chrono::seconds(15)),
                "concurrent response did not arrive");
    {
        std::lock_guard lock(results_mutex);
        TEST_EQUAL(results.size(), 32, "response callback count changed");
        for (const Slice& slice : results) {
            TEST_REQUIRE(slice && slice.size_bytes() == 1024, "concurrent exchange failed");
            TEST_EQUAL((slice.data<unsigned char>()[1023] ^ 0xA5), slice.data<unsigned char>()[0],
                    "concurrent response corruption");
        }
        results.clear();
    }
    SliceChannel::close(number, selected);
}
/** --------------------------------------------------------------------------------------------------------- Wire Validation
 * @brief Exercises the real decoder against tampered authentication, mismatched modes, and corrupt lengths.
 */
void wire_validation() {
    unsigned char key[32], token[16]{};
    TEST_REQUIRE(!ba_net_key(key), "test key invalid");
    Slice source(123);
    std::memset(source.raw(), 0x42, source.size_bytes());
    ba_net_frame frame{};
    TEST_REQUIRE(!ba_net_encode(reinterpret_cast<const ba_slice_t*>(&source), BA_NET_SECURE, token, key,
                           &frame),
            "frame encoding failed");
    ba_slice_t decoded;
    TEST_REQUIRE(!ba_net_decode(frame.data, frame.size, 128, key, &decoded),
            "authenticated frame rejected");
    TEST_EQUAL(std::memcmp(decoded.ptr, source.raw(), source.size_bytes()), 0,
            "decoded bytes differ");
    ba_release(&decoded);
    frame.data[frame.size - 1] ^= 1;
    TEST_EQUAL(ba_net_decode(frame.data, frame.size, 128, key, &decoded), UV_EACCES,
            "forged tag accepted");
    frame.data[frame.size - 1] ^= 1;
    TEST_EQUAL(ba_net_decode(frame.data, frame.size, 0, key, &decoded), UV_EACCES,
            "encryption downgrade accepted");
    frame.data[16] ^= 1;
    TEST_EQUAL(ba_net_decode(frame.data, frame.size, 128, key, &decoded), UV_EACCES,
            "forged request identifier accepted");
    frame.data[16] ^= 1;
    frame.data[8] = 0xFF;
    size_t bytes;
    TEST_EQUAL(ba_net_frame_size(frame.data, &bytes), UV_EMSGSIZE, "oversized length accepted");
    free(frame.data);
}
/** --------------------------------------------------------------------------------------------------------- Failure Contracts
 * @brief Checks invalid reply scope, bind failures, datagram limits, and canceled response delivery.
 */
void failures() {
    bool rejected = false;
    try {
        SliceChannel::send(Slice(1), "", 32000, Protocol::TCP);
    } catch (const std::exception&) {
        rejected = true;
    }
    TEST_REQUIRE(rejected, "empty address accepted outside receive callback");
    port = unused_port();
    protocol = Protocol::UDP;
    SliceChannel::listen(port, protocol, idle);
    rejected = false;
    try {
        SliceChannel::listen(port, protocol, idle);
    } catch (const std::exception&) {
        rejected = true;
    }
    TEST_REQUIRE(rejected, "duplicate listener accepted");
    rejected = false;
    try {
        SliceChannel::send(Slice(65507), "127.0.0.1", port, protocol);
    } catch (const std::exception&) {
        rejected = true;
    }
    TEST_REQUIRE(rejected, "oversized datagram accepted");
    SliceChannel::send(Slice(100), "127.0.0.1", port, protocol, response);
    SliceChannel::close(port, protocol);
    TEST_REQUIRE(completed.try_acquire_for(std::chrono::seconds(5)),
            "close did not cancel pending response");
    {
        std::lock_guard lock(results_mutex);
        TEST_REQUIRE(results.size() == 1 && !results[0],
                "canceled response did not report null exactly once");
        results.clear();
    }
    SliceChannel::listen(port, protocol, idle);
    SliceChannel::close(port, protocol);
    SliceChannel::close(port, protocol);
    port = unused_port();
    SliceChannel::listen(port, protocol, receive_oneway);
    SliceChannel::send(Slice(100), "127.0.0.1", port, protocol);
    TEST_REQUIRE(completed.try_acquire_for(std::chrono::seconds(5)), "one-way receive callback failed");
    SliceChannel::close(port, protocol);
}
} // namespace
int main(int count, char** arguments) {
    test_support::start(__FILE__);
    try {
        setenv("ALLIGATOR_NETWORK_KEY",
               "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", 1);
        if (count == 6 && !std::strcmp(arguments[1], "server")) {
            protocol = static_cast<Protocol>(std::stoi(arguments[2]));
            port = static_cast<uint16_t>(std::stoi(arguments[3]));
            register_placement("Padding");
            register_placement("NetworkTest");
            SliceChannel::listen(port, protocol, receive);
            const char ready = 'R';
            TEST_EQUAL(write(std::stoi(arguments[4]), &ready, 1), 1, "ready signal failed");
            ::close(std::stoi(arguments[4]));
            char command;
            (void)read(std::stoi(arguments[5]), &command, 1);
            SliceChannel::close(port, protocol);
            BuffetMenu::shutdown();
            return 0;
        }
        register_placement("NetworkTest");
        SliceT<uint64_t> typed(size_t{19});
        TEST_EQUAL(typed.size_bytes(), sizeof(uint64_t) * 19,
                "SliceT count constructor allocated wrong size");
        wire_validation();
        for (Protocol selected : {Protocol::TCP, Protocol::UDP, Protocol::EncryptedTCP,
                                  Protocol::EncryptedUDP, Protocol::RDMA, Protocol::EncryptedRDMA})
            round_trips(arguments[0], selected);
        failures();
        BuffetMenu::shutdown();
        std::puts("Network contracts passed");
    } catch (const std::exception& error) {
        test_support::fail("Unexpected exception; source is the last test checkpoint",
            test_support::last_operation, "successful completion", error.what(), test_support::last_location);
    }
}
