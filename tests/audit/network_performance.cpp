/** --------------------------------------------------------------------------------------------------------- Network Performance
 * @file network_performance.cpp
 * @brief Measures validated request-response latency across separate processes through the public API.
 */
#include <alligator.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <semaphore>
#include <string_view>
#include <vector>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>

namespace {
using namespace buffetalligator;
using Protocol = SliceChannel::Protocol;
using Clock = std::chrono::steady_clock;
Protocol protocol;
uint16_t port;
std::binary_semaphore completed{0};
bool valid_response;
size_t payload_bytes;
uint64_t expected_sequence;
/** --------------------------------------------------------------------------------------------------------- Require
 * @brief Reports an invalid workload or failed exchange.
 */
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
/** --------------------------------------------------------------------------------------------------------- Receive
 * @brief Changes the final payload byte and replies over the sender's connection.
 */
void receive(Slice slice) {
    slice.data()[slice.size_bytes() - 1] ^= 0xA5;
    SliceChannel::send(std::move(slice), "", port, protocol);
}
/** --------------------------------------------------------------------------------------------------------- Response
 * @brief Validates the received sequence and every payload byte before completing the request.
 */
void response(Slice slice) {
    valid_response = slice && slice.size_bytes() == payload_bytes &&
        slice.data<uint64_t>()[0] == expected_sequence;
    if (valid_response) {
        for (size_t index = sizeof(uint64_t); index < payload_bytes; ++index) {
            unsigned char expected = static_cast<unsigned char>(index % 251);
            if (index + 1 == payload_bytes) expected ^= 0xA5;
            if (slice.data()[index] != expected) valid_response = false;
        }
    }
    completed.release();
}
/** --------------------------------------------------------------------------------------------------------- Port
 * @brief Obtains an available local port for an isolated benchmark server.
 */
uint16_t unused_port() {
    const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
    require(descriptor >= 0, "port probe failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(!bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)), "port bind failed");
    socklen_t bytes = sizeof(address);
    require(!getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &bytes), "port query failed");
    ::close(descriptor);
    return ntohs(address.sin_port);
}
/** --------------------------------------------------------------------------------------------------------- Server
 * @brief Owns a separate executable instance until its control pipe is closed.
 */
struct Server {
    pid_t process;
    int control;
    /** ------------------------------------------------------------------------------------------- Start
     * @brief Waits until the child's listener is ready before timing any exchanges.
     */
    Server(const char* executable, Protocol selected, uint16_t number) {
        int ready[2], commands[2];
        require(!pipe(ready) && !pipe(commands), "server pipes failed");
        const std::string selected_text = std::to_string(static_cast<unsigned>(selected));
        const std::string port_text = std::to_string(number);
        const std::string ready_text = std::to_string(ready[1]);
        const std::string control_text = std::to_string(commands[0]);
        process = fork();
        require(process >= 0, "server fork failed");
        if (!process) {
            ::close(ready[0]);
            ::close(commands[1]);
            execl(executable, executable, "server", selected_text.c_str(), port_text.c_str(),
                ready_text.c_str(), control_text.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        ::close(ready[1]);
        ::close(commands[0]);
        control = commands[1];
        char message = 0;
        const ssize_t received = read(ready[0], &message, 1);
        ::close(ready[0]);
        require(received == 1 && message == 'R', "server listener did not start");
    }
    /** ------------------------------------------------------------------------------------------- Stop
     * @brief Waits for the child's ordered channel and arena shutdown.
     */
    ~Server() {
        ::close(control);
        int status = 0;
        if (waitpid(process, &status, 0) != process || !WIFEXITED(status) || WEXITSTATUS(status))
            std::abort();
    }
};
/** --------------------------------------------------------------------------------------------------------- Exchange
 * @brief Sends one source Slice and waits for its validated callback.
 */
void exchange(Slice& source, uint64_t sequence) {
    expected_sequence = sequence;
    source.data<uint64_t>()[0] = sequence;
    SliceChannel::send(source, "127.0.0.1", port, protocol, response);
    require(completed.try_acquire_for(std::chrono::seconds(10)), "response exceeded benchmark deadline");
    require(valid_response, "response payload validation failed");
}
/** --------------------------------------------------------------------------------------------------------- Measure
 * @brief Measures established listener traffic with one outstanding request and a reusable source Slice.
 */
void measure(const char* executable, const char* label, Protocol selected, size_t bytes,
    size_t operations, unsigned repetition) {
    protocol = selected;
    port = unused_port();
    Server server(executable, protocol, port);
    payload_bytes = bytes;
    Slice source(bytes);
    for (size_t index = 0; index < bytes; ++index) source.data()[index] = static_cast<unsigned char>(index % 251);
    for (size_t index = 0; index < 32; ++index) exchange(source, index);
    std::vector<uint64_t> latency(operations);
    const auto wall = Clock::now();
    for (size_t index = 0; index < operations; ++index) {
        const auto begin = Clock::now();
        exchange(source, index + 32);
        latency[index] = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
    }
    const uint64_t elapsed = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - wall).count());
    std::sort(latency.begin(), latency.end());
    const auto percentile = [&](size_t value) { return latency[(latency.size() - 1) * value / 100]; };
    std::printf("%s,%u,%zu,%zu,%llu,%.3f,%.3f,%llu,%llu,%llu,%llu\n", label, repetition,
        bytes, operations, (unsigned long long)elapsed, operations * 1e9 / elapsed,
        operations * static_cast<double>(bytes) * 2e9 / elapsed / (1024 * 1024),
        (unsigned long long)percentile(50), (unsigned long long)percentile(95),
        (unsigned long long)percentile(99), (unsigned long long)latency.back());
    SliceChannel::close(port, protocol);
}
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs TCP, UDP, and libfabric request-response rows with optional encryption.
 */
int main(int count, char** arguments) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    try {
        setenv("ALLIGATOR_NETWORK_KEY", "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", 1);
        setenv("FI_PROVIDER", "tcp", 1);
        if (count == 6 && std::string_view(arguments[1]) == "server") {
            protocol = static_cast<Protocol>(std::stoul(arguments[2]));
            port = static_cast<uint16_t>(std::stoul(arguments[3]));
            SliceChannel::listen(port, protocol, receive);
            const char ready = 'R';
            require(write(std::stoi(arguments[4]), &ready, 1) == 1, "server ready signal failed");
            ::close(std::stoi(arguments[4]));
            char command;
            (void)read(std::stoi(arguments[5]), &command, 1);
            ::close(std::stoi(arguments[5]));
            SliceChannel::close(port, protocol);
            BuffetMenu::shutdown();
            return 0;
        }
        const unsigned repetitions = count > 1 ? static_cast<unsigned>(std::stoul(arguments[1])) : 3;
        std::puts("protocol,repetition,payload_bytes,round_trips,elapsed_ns,round_trips_per_second,payload_mib_per_second,p50_ns,p95_ns,p99_ns,max_ns");
        const Protocol protocols[] = {Protocol::TCP, Protocol::UDP, Protocol::RDMA,
            Protocol::EncryptedTCP, Protocol::EncryptedUDP, Protocol::EncryptedRDMA};
        const char* labels[] = {"TCP", "UDP", "libfabric_tcp", "EncryptedTCP", "EncryptedUDP", "Encrypted_libfabric_tcp"};
        for (unsigned repetition = 1; repetition <= repetitions; ++repetition) {
            for (size_t index = 0; index < 6; ++index) {
                for (size_t bytes : {1024u, 32768u}) {
                    std::fprintf(stderr, "Measuring network %s bytes=%zu repetition=%u/%u\n",
                        labels[index], bytes, repetition, repetitions);
                    measure(arguments[0], labels[index], protocols[index], bytes, 1000, repetition);
                }
            }
        }
        BuffetMenu::shutdown();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Network benchmark failed: %s\n", error.what());
        return 1;
    }
}
