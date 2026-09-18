/** --------------------------------------------------------------------------------------------------------- Network Lifecycle Tests
 * @file network_lifecycle_test.cpp
 * @brief Exercises delivery, receiver challenges, and exchange deadlines through actual sockets.
 */
#include <alligator.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <vector>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
extern "C" {
#include "network/ba_network.h"
}

using namespace buffetalligator;
namespace {
using Protocol = SliceChannel::Protocol;
using namespace std::chrono_literals;
std::counting_semaphore<4096> received{0};
std::counting_semaphore<4096> responses{0};
std::atomic<unsigned> invalid_payloads{0};
std::atomic<unsigned> valid_responses{0};
/** --------------------------------------------------------------------------------------------------------- Require
 * @brief Stops a test immediately when an actual channel contract fails.
 */
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
/** --------------------------------------------------------------------------------------------------------- Socket
 * @brief Owns a test socket independently of the library's transport handles.
 */
struct Socket {
    int descriptor;
    explicit Socket(int type) : descriptor(socket(AF_INET, type, 0)) {
        require(descriptor >= 0, "socket creation failed");
    }
    Socket(const Socket&) = delete;
    ~Socket() { ::close(descriptor); }
    /** ------------------------------------------------------------------------------------------- Bind
     * @brief Binds a temporary loopback port for a listener or silent peer.
     */
    uint16_t bind_port() {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
                "socket bind failed");
        socklen_t length = sizeof(address);
        require(getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &length) == 0,
                "socket name failed");
        return ntohs(address.sin_port);
    }
    /** ------------------------------------------------------------------------------------------- Connect
     * @brief Connects to a real library listener on loopback.
     */
    void connect_port(uint16_t port) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        require(connect(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
                "socket connect failed");
    }
    /** ------------------------------------------------------------------------------------------- Write
     * @brief Sends all bytes while retaining stream fragmentation chosen by the test.
     */
    void write_bytes(const unsigned char* bytes, size_t size) {
        while (size) {
            const ssize_t written = send(descriptor, bytes, size, 0);
            require(written > 0, "socket write failed");
            bytes += written;
            size -= static_cast<size_t>(written);
        }
    }
    /** ------------------------------------------------------------------------------------------- Read
     * @brief Waits for bounded socket input without allowing a regression to hang the suite.
     */
    ssize_t read_bytes(unsigned char* bytes, size_t size) {
        pollfd pending{descriptor, POLLIN, 0};
        require(poll(&pending, 1, 2000) == 1, "socket read deadline exceeded");
        return recv(descriptor, bytes, size, 0);
    }
};
/** --------------------------------------------------------------------------------------------------------- Port
 * @brief Obtains a temporary listener port from the operating system.
 */
uint16_t unused_port() {
    Socket socket(SOCK_STREAM);
    return socket.bind_port();
}
/** --------------------------------------------------------------------------------------------------------- Receive
 * @brief Validates every payload byte before recording a delivered one-way message.
 */
void receive(Slice slice) {
    if (!slice || !slice.size_bytes()) invalid_payloads.fetch_add(1, std::memory_order_relaxed);
    else {
        const auto* bytes = slice.data<unsigned char>();
        for (size_t index = 0; index < slice.size_bytes(); ++index) {
            if (bytes[index] != 0x42) {
                invalid_payloads.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    }
    received.release();
}
/** --------------------------------------------------------------------------------------------------------- Response
 * @brief Records timeout and cancellation callbacks without inspecting null Slice metadata.
 */
void response(Slice slice) {
    if (slice) valid_responses.fetch_add(1, std::memory_order_relaxed);
    responses.release();
}
/** --------------------------------------------------------------------------------------------------------- One Way
 * @brief Reproduces libfabric teardown loss for concurrent and fragmented application frames.
 */
void one_way(Protocol protocol) {
    const uint16_t port = unused_port();
    SliceChannel::listen(port, protocol, receive);
    Slice source(32);
    std::memset(source.raw(), 0x42, source.size_bytes());
    const unsigned workers = std::clamp(std::thread::hardware_concurrency() * 2, 16u, 64u);
    std::vector<std::thread> producers;
    for (unsigned index = 0; index < workers; ++index)
        producers.emplace_back([&] { SliceChannel::send(source, "127.0.0.1", port, protocol); });
    for (auto& producer : producers) producer.join();
    for (unsigned index = 0; index < workers; ++index)
        require(received.try_acquire_for(5s), "one-way message was lost");
    const bool datagram = (static_cast<unsigned>(protocol) & 127) == 1;
    Slice large(datagram ? 48001 : 2097155);
    std::memset(large.raw(), 0x42, large.size_bytes());
    for (unsigned message = 0; message < 4; ++message) {
        SliceChannel::send(large, "127.0.0.1", port, protocol);
        require(received.try_acquire_for(5s), "fragmented one-way message was lost");
    }
    SliceChannel::close(port, protocol);
    require(!received.try_acquire(), "one-way message was delivered twice");
    require(!invalid_payloads.load(), "one-way payload was corrupted");
    std::printf("One-way protocol %u: %u concurrent and 4 large messages passed\n",
                static_cast<unsigned>(protocol), workers);
    std::fflush(stdout);
}
/** --------------------------------------------------------------------------------------------------------- Challenge
 * @brief Obtains and verifies a fresh challenge using the real private framing functions.
 */
void challenge(
    Socket& socket, const unsigned char key[32], unsigned char token[16], bool datagram
) {
    unsigned char client[16], empty[16]{};
    randombytes_buf(client, sizeof(client));
    ba_net_frame hello{};
    require(!ba_net_control_encode(BA_NET_HELLO, client, empty, key, &hello),
            "hello encode failed");
    if (!datagram) {
        socket.write_bytes(hello.data, 13);
        socket.write_bytes(hello.data + 13, hello.size - 13);
    } else socket.write_bytes(hello.data, hello.size);
    free(hello.data);
    unsigned char reply[BA_NET_HEADER + 32];
    size_t received_bytes = 0;
    while (received_bytes < sizeof(reply)) {
        ssize_t bytes = socket.read_bytes(reply + received_bytes, sizeof(reply) - received_bytes);
        require(bytes > 0, "challenge missing");
        received_bytes += static_cast<size_t>(bytes);
        if (datagram) require(received_bytes == sizeof(reply), "challenge datagram size changed");
    }
    require(!sodium_memcmp(reply + 16, client, 16), "challenge correlation changed");
    require(!ba_net_control_decode(reply, sizeof(reply), key, token),
            "challenge authentication failed");
    reply[sizeof(reply) - 1] ^= 1;
    unsigned char rejected[16];
    require(ba_net_control_decode(reply, sizeof(reply), key, rejected) == UV_EACCES,
            "forged challenge accepted");
}
/** --------------------------------------------------------------------------------------------------------- Replay
 * @brief Rejects captured requests across repeated datagrams, fresh connections, and listener restarts.
 */
void replay(bool datagram) {
    const Protocol protocol = datagram ? Protocol::EncryptedUDP : Protocol::EncryptedTCP;
    const uint16_t port = unused_port();
    unsigned char key[32], token[16];
    require(!ba_net_key(key), "key configuration failed");
    Slice source(123);
    std::memset(source.raw(), 0x42, source.size_bytes());
    ba_net_frame captured{};
    SliceChannel::listen(port, protocol, receive);
    {
        Socket socket(datagram ? SOCK_DGRAM : SOCK_STREAM);
        socket.connect_port(port);
        challenge(socket, key, token, datagram);
        require(!ba_net_encode(reinterpret_cast<const ba_slice_t*>(&source), BA_NET_SECURE,
                               token, key, &captured), "request encode failed");
        socket.write_bytes(captured.data, captured.size);
        require(received.try_acquire_for(2s), "fresh authenticated request was rejected");
        if (datagram) {
            socket.write_bytes(captured.data, captured.size);
            require(!received.try_acquire_for(100ms), "encrypted datagram replay was delivered");
        }
    }
    for (unsigned restart = 0; restart < 2; ++restart) {
        if (restart) {
            SliceChannel::close(port, protocol);
            SliceChannel::listen(port, protocol, receive);
        }
        Socket socket(datagram ? SOCK_DGRAM : SOCK_STREAM);
        socket.connect_port(port);
        unsigned char replacement[16];
        challenge(socket, key, replacement, datagram);
        require(sodium_memcmp(token, replacement, 16) != 0, "receiver challenge was reused");
        socket.write_bytes(captured.data, captured.size);
        require(!received.try_acquire_for(100ms),
                "request replay crossed a connection or restart");
    }
    {
        Socket socket(datagram ? SOCK_DGRAM : SOCK_STREAM);
        socket.connect_port(port);
        challenge(socket, key, token, datagram);
        ba_net_frame fresh{};
        require(!ba_net_encode(reinterpret_cast<const ba_slice_t*>(&source), BA_NET_SECURE,
                               token, key, &fresh), "fresh request encode failed");
        socket.write_bytes(fresh.data, fresh.size);
        free(fresh.data);
        require(received.try_acquire_for(2s), "fresh request failed after rejected replays");
    }
    free(captured.data);
    SliceChannel::close(port, protocol);
    std::printf("Encrypted %s replay and listener restart checks passed\n",
                datagram ? "UDP" : "TCP");
    std::fflush(stdout);
}
/** --------------------------------------------------------------------------------------------------------- Expired Challenge
 * @brief Rejects a valid request whose receiver nonce expired before it was used.
 */
void expired_challenge() {
    const uint16_t port = unused_port();
    require(!ba_net_listen(port, static_cast<uint8_t>(Protocol::EncryptedUDP),
        reinterpret_cast<ba_net_callback>(receive), 100), "challenge listener failed");
    unsigned char key[32], token[16];
    require(!ba_net_key(key), "key configuration failed");
    Socket socket(SOCK_DGRAM);
    socket.connect_port(port);
    challenge(socket, key, token, true);
    require(!received.try_acquire_for(200ms), "challenge reached the application");
    Slice source(1);
    ba_net_frame frame{};
    require(!ba_net_encode(reinterpret_cast<const ba_slice_t*>(&source), BA_NET_SECURE,
                           token, key, &frame), "expired request encode failed");
    socket.write_bytes(frame.data, frame.size);
    free(frame.data);
    require(!received.try_acquire_for(100ms), "expired challenge was accepted");
    SliceChannel::close(port, Protocol::EncryptedUDP);
}
/** --------------------------------------------------------------------------------------------------------- Deadlines
 * @brief Completes silent peers exactly once and closes stalled inbound streams.
 */
void deadlines() {
    for (Protocol protocol : {Protocol::TCP, Protocol::UDP, Protocol::EncryptedTCP,
                              Protocol::EncryptedUDP}) {
        const bool datagram = (static_cast<unsigned>(protocol) & 127) == 1;
        Socket silent(datagram ? SOCK_DGRAM : SOCK_STREAM);
        const uint16_t port = silent.bind_port();
        if (!datagram) require(listen(silent.descriptor, 8) == 0, "silent listener failed");
        Slice source(1);
        require(!ba_net_send(reinterpret_cast<const ba_slice_t*>(&source), "127.0.0.1", port,
            static_cast<uint8_t>(protocol), reinterpret_cast<ba_net_callback>(response), 100),
            "silent peer send failed");
        require(responses.try_acquire_for(2s), "silent peer did not time out");
        SliceChannel::close(port, protocol);
        require(!responses.try_acquire_for(150ms), "timeout callback was delivered twice");
    }
    require(!valid_responses.load(), "timeout produced a valid response");
    for (Protocol protocol : {Protocol::TCP, Protocol::EncryptedTCP}) {
        const uint16_t port = unused_port();
        require(!ba_net_listen(port, static_cast<uint8_t>(protocol),
            reinterpret_cast<ba_net_callback>(receive), 100), "stalled listener failed");
        Socket stalled(SOCK_STREAM);
        stalled.connect_port(port);
        const unsigned char partial[] = {'B', 'A', 'G'};
        stalled.write_bytes(partial, sizeof(partial));
        unsigned char byte;
        require(stalled.read_bytes(&byte, 1) == 0, "stalled inbound stream did not expire");
        SliceChannel::close(port, protocol);
    }
    Slice source(1);
    require(ba_net_send(reinterpret_cast<const ba_slice_t*>(&source), "127.0.0.1", unused_port(),
        static_cast<uint8_t>(Protocol::TCP), reinterpret_cast<ba_net_callback>(response), 0)
        == UV_EINVAL, "zero exchange deadline accepted");
    std::puts("Silent-peer and partial-frame deadlines passed");
}
} // namespace
int main() {
    setenv("ALLIGATOR_NETWORK_KEY",
           "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", 1);
    try {
        for (Protocol protocol : {Protocol::TCP, Protocol::UDP, Protocol::EncryptedTCP,
                                  Protocol::EncryptedUDP, Protocol::RDMA, Protocol::EncryptedRDMA})
            one_way(protocol);
        replay(true);
        replay(false);
        expired_challenge();
        deadlines();
        BuffetMenu::shutdown();
        std::puts("Network lifecycle contracts passed");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Network lifecycle contracts failed: %s\n", error.what());
        return 1;
    }
}
