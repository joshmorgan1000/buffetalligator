/** --------------------------------------------------------------------------------------------------------- Network Contract Tests
 * @file network_test.cpp
 * @brief Verifies public Slice channels across processes and rejects malformed or forged private frames.
 */
#include <alligator.hpp>
#include <alligator/containers.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
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
/** --------------------------------------------------------------------------------------------------------- Require
 * @brief Stops the test on a violated public contract.
 */
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
/** --------------------------------------------------------------------------------------------------------- Test Placement
 * @brief Genuine registered heap storage whose placement name differs between the two processes.
 */
template<int Slot>
struct alignas(16) TestPlacement {
    void* memory = nullptr;  ///< The allocated block.
    size_t bytes = 0;        ///< The block size in bytes.
    inline static const char* name = "Unnamed";
    TestPlacement(size_t size = 0) : bytes(size) {
        if (size == 0) return;
        if (posix_memalign(&memory, 64, size)) throw std::bad_alloc();
        std::memset(memory, 0, size);
    }
    TestPlacement(const TestPlacement&) = delete;
    TestPlacement& operator=(const TestPlacement&) = delete;
    TestPlacement(TestPlacement&& other) noexcept
    : memory(std::exchange(other.memory, nullptr)), bytes(std::exchange(other.bytes, 0)) {}
    TestPlacement& operator=(TestPlacement&& other) noexcept {
        std::swap(memory, other.memory); std::swap(bytes, other.bytes); return *this;
    }
    ~TestPlacement() { std::free(memory); }
    operator bool() const { return memory != nullptr; }
    bool operator==(std::nullptr_t) const { return memory == nullptr; }
    static void deleter_impl(void* handle) { delete static_cast<TestPlacement*>(handle); }
    static void (*deleter())(void*) { return deleter_impl; }
    static void* host_ptr(void* handle) { return static_cast<TestPlacement*>(handle)->memory; }
    static size_t size_of(void* handle) { return static_cast<TestPlacement*>(handle)->bytes; }
    static void* factory(size_t size) { return new TestPlacement(size); }
    static size_t default_size() { return 256u * 1024u * 1024u; }
    static size_t* type_idx() { static size_t idx = 0; return &idx; }
    static const char* type_name() { return name; }
    static uint64_t device_address(void* handle) { return reinterpret_cast<uint64_t>(host_ptr(handle)); }
    size_t size() const { return bytes; }
    void* raw() { return memory; }
    const void* raw() const { return memory; }
};
const BuffetDescriptor* network_placement = nullptr;
/** --------------------------------------------------------------------------------------------------------- Register
 * @brief Registers one named placement for cross-process identifier tests.
 */
void register_placement(const char* name) {
    static_cast<void>(Slice::default_placement());
    static size_t registered = 0;
    const BuffetDescriptor* descriptor = nullptr;
    if (registered++ == 0) {
        TestPlacement<0>::name = name;
        descriptor = BuffetDescriptors::descriptor<TestPlacement<0>>();
    } else {
        TestPlacement<1>::name = name;
        descriptor = BuffetDescriptors::descriptor<TestPlacement<1>>();
    }
    if (std::strcmp(name, "NetworkTest") == 0) network_placement = descriptor;
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
    require(descriptor >= 0, "test socket creation failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "test bind failed");
    socklen_t length = sizeof(address);
    require(getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &length) == 0,
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
        require(pipe(ready) == 0 && pipe(commands) == 0, "test pipes failed");
        std::string protocol_text = std::to_string(static_cast<unsigned>(selected));
        std::string port_text = std::to_string(number);
        std::string ready_text = std::to_string(ready[1]);
        std::string command_text = std::to_string(commands[0]);
        process = fork();
        require(process >= 0, "test fork failed");
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
/** --------------------------------------------------------------------------------------------------------- Exercise Peer
 * @brief Checks multi-threaded payload, subview, novel backing, and placement preservation.
 */
void exercise_peer(const char* address, const char* concurrent_address, uint16_t number,
                    Protocol selected) {
    std::printf("Testing protocol %u across processes on port %u\n",
                static_cast<unsigned>(selected), number);
    const bool datagram = (static_cast<unsigned>(selected) & 127) == 1;
    const size_t sizes[] = {1, 8193, datagram ? 48001u : 2097155u};
    for (size_t bytes : sizes) {
        Slice backing(bytes + 13, true, network_placement);
        Slice view = backing.slice(7, bytes);
        const size_t represented_bytes = view.size_bytes();
        for (size_t index = 0; index < represented_bytes; ++index)
            view.data<unsigned char>()[index] = index % 251;
        backing.free();
        const auto sole_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!view.is_novel() && std::chrono::steady_clock::now() < sole_deadline) std::this_thread::yield();
        require(view.is_novel(), "the view did not become the sole owner of its novel backing");
        SliceChannel::send(std::move(view), address, number, selected, response);
        require(completed.try_acquire_for(std::chrono::seconds(15)), "response did not arrive");
        Slice result;
        {
            std::lock_guard lock(results_mutex);
            result = std::move(results.back());
            results.pop_back();
        }
        require(result && result.size_bytes() == represented_bytes, "response length changed");
        require(result.is_novel(), "novel backing was lost");
        require(std::strcmp(result.placement()->type_name_fn(), "NetworkTest") == 0,
                "placement was not preserved by name");
        for (size_t index = 0; index < represented_bytes; ++index) {
            unsigned expected = index % 251;
            if (index + 1 == represented_bytes) expected ^= 0xA5;
            require(result.data<unsigned char>()[index] == expected,
                    "payload changed across the wire");
        }
    }
    std::vector<std::thread> workers;
    for (unsigned index = 0; index < 8; ++index)
        workers.emplace_back([=] {
            for (unsigned message = 0; message < 4; ++message) {
                Slice slice(1024);
                std::memset(slice.raw(), index + message + 1, slice.size_bytes());
                SliceChannel::send(std::move(slice), concurrent_address, number, selected, response);
            }
        });
    for (auto& worker : workers) worker.join();
    for (unsigned index = 0; index < 32; ++index)
        require(completed.try_acquire_for(std::chrono::seconds(15)),
                "concurrent response did not arrive");
    {
        std::lock_guard lock(results_mutex);
        require(results.size() == 32, "response callback count changed");
        for (const Slice& slice : results) {
            require(slice && slice.size_bytes() == 1024, "concurrent exchange failed");
            require((slice.data<unsigned char>()[1023] ^ 0xA5) == slice.data<unsigned char>()[0],
                    "concurrent response corruption");
        }
        results.clear();
    }
    SliceChannel::close(number, selected);
}
/** --------------------------------------------------------------------------------------------------------- Round Trips
 * @brief Runs the peer contract with a separately executed local server.
 */
void round_trips(const char* executable, Protocol selected) {
    const uint16_t number = unused_port();
    Child server(executable, selected, number);
    exercise_peer("127.0.0.1", "localhost", number, selected);
}
/** --------------------------------------------------------------------------------------------------------- Wire Validation
 * @brief Exercises the real decoder against tampered authentication, mismatched modes, and corrupt lengths.
 */
void wire_validation() {
    unsigned char key[32], token[16]{};
    require(!ba_net_key(key), "test key invalid");
    Slice source(123);
    std::memset(source.raw(), 0x42, source.size_bytes());
    ba_net_frame frame{};
    require(!ba_net_encode(reinterpret_cast<const ba_slice_t*>(&source), BA_NET_SECURE, token, key,
                           &frame),
            "frame encoding failed");
    ba_slice_t decoded;
    require(!ba_net_decode(frame.data, frame.size, 128, key, &decoded),
            "authenticated frame rejected");
    require(std::memcmp(ba_slice_ptr(&decoded), source.raw(), source.size_bytes()) == 0,
            "decoded bytes differ");
    ba_release(&decoded);
    frame.data[frame.size - 1] ^= 1;
    require(ba_net_decode(frame.data, frame.size, 128, key, &decoded) == UV_EACCES,
            "forged tag accepted");
    frame.data[frame.size - 1] ^= 1;
    require(ba_net_decode(frame.data, frame.size, 0, key, &decoded) == UV_EACCES,
            "encryption downgrade accepted");
    frame.data[16] ^= 1;
    require(ba_net_decode(frame.data, frame.size, 128, key, &decoded) == UV_EACCES,
            "forged request identifier accepted");
    frame.data[16] ^= 1;
    frame.data[8] = 0xFF;
    size_t bytes;
    require(ba_net_frame_size(frame.data, &bytes) == UV_EMSGSIZE, "oversized length accepted");
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
    require(rejected, "empty address accepted outside receive callback");
    port = unused_port();
    protocol = Protocol::UDP;
    SliceChannel::listen(port, protocol, idle);
    rejected = false;
    try {
        SliceChannel::listen(port, protocol, idle);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "duplicate listener accepted");
    rejected = false;
    try {
        SliceChannel::send(Slice(65507), "127.0.0.1", port, protocol);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "oversized datagram accepted");
    SliceChannel::send(Slice(100), "127.0.0.1", port, protocol, response);
    SliceChannel::close(port, protocol);
    require(completed.try_acquire_for(std::chrono::seconds(5)),
            "close did not cancel pending response");
    {
        std::lock_guard lock(results_mutex);
        require(results.size() == 1 && !results[0],
                "canceled response did not report null exactly once");
        results.clear();
    }
    SliceChannel::listen(port, protocol, idle);
    SliceChannel::close(port, protocol);
    SliceChannel::close(port, protocol);
    port = unused_port();
    SliceChannel::listen(port, protocol, receive_oneway);
    SliceChannel::send(Slice(100), "127.0.0.1", port, protocol);
    require(completed.try_acquire_for(std::chrono::seconds(5)), "one-way receive callback failed");
    SliceChannel::close(port, protocol);
}
} // namespace
int main(int count, char** arguments) {
    try {
        setenv("ALLIGATOR_NETWORK_KEY",
               "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", 1);
        const bool remote_server = count == 4 && !std::strcmp(arguments[1], "--listen");
        if ((count == 6 && !std::strcmp(arguments[1], "server")) || remote_server) {
            protocol = static_cast<Protocol>(std::stoi(arguments[2]));
            port = static_cast<uint16_t>(std::stoi(arguments[3]));
            register_placement("Padding");
            register_placement("NetworkTest");
            SliceChannel::listen(port, protocol, receive);
            if (remote_server) {
                std::printf("Protocol %u ready on port %u; press Enter to stop\n",
                            static_cast<unsigned>(protocol), port);
                std::fflush(stdout);
                (void)std::getchar();
            } else {
                const char ready = 'R';
                require(write(std::stoi(arguments[4]), &ready, 1) == 1, "ready signal failed");
                ::close(std::stoi(arguments[4]));
                char command;
                (void)read(std::stoi(arguments[5]), &command, 1);
            }
            SliceChannel::close(port, protocol);
            ba_net_shutdown();
            return 0;
        }
        register_placement("NetworkTest");
        if (count == 5 && !std::strcmp(arguments[1], "--connect")) {
            protocol = static_cast<Protocol>(std::stoi(arguments[2]));
            port = static_cast<uint16_t>(std::stoi(arguments[3]));
            exercise_peer(arguments[4], arguments[4], port, protocol);
            ba_net_shutdown();
            std::puts("Remote network contracts passed");
            return 0;
        }
        struct alignas(64) TestStruct {
            uint64_t data;
        };
        SliceT<TestStruct> typed(true);
        require(typed.size_bytes() == sizeof(TestStruct), "SliceT claimed the wrong size for one element");
        wire_validation();
        for (Protocol selected : {Protocol::TCP, Protocol::UDP, Protocol::EncryptedTCP,
                                  Protocol::EncryptedUDP, Protocol::RDMA, Protocol::EncryptedRDMA})
            round_trips(arguments[0], selected);
        failures();
        ba_net_shutdown();
        std::puts("Network contracts passed");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Network contracts failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
