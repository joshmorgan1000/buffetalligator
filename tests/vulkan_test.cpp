/** --------------------------------------------------------------------------------------------------------- Vulkan Network Contract
 * @file vulkan_test.cpp
 * @brief Verifies Vulkan property preservation and byte-exact views against a separately initialized receiver.
 */
#include <alligator.hpp>
#include <cstdio>
#include <cstdlib>
#include <semaphore>
#include <stdexcept>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
extern "C" {
#include "memory/ba_vulkan.h"
}

using namespace buffetalligator;
namespace {
uint16_t port;
Slice received;
std::binary_semaphore completed{0};
/** --------------------------------------------------------------------------------------------------------- Require
 * @brief Checks GPU and network contracts in optimized builds.
 */
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
/** --------------------------------------------------------------------------------------------------------- Response
 * @brief Publishes the received ownership before waking the test thread.
 */
void response(Slice slice) {
    received = std::move(slice);
    completed.release();
}
/** --------------------------------------------------------------------------------------------------------- Receive
 * @brief Confirms device storage exists before replying from the received host view.
 */
void receive(Slice slice) {
    require(slice.vulkan_buffer().buffer != VK_NULL_HANDLE, "receiver has no Vulkan buffer");
    slice.data<unsigned char>()[slice.size_bytes() - 1] ^= 0xA5;
    slice.vulkan_sync(true);
    SliceChannel::send(std::move(slice), "", port, SliceChannel::Protocol::TCP);
}
/** --------------------------------------------------------------------------------------------------------- Free Port
 * @brief Chooses a currently available TCP service for the isolated receiver.
 */
uint16_t unused_port() {
    int descriptor = socket(AF_INET, SOCK_STREAM, 0);
    require(descriptor >= 0, "test socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(!bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)),
            "test bind failed");
    socklen_t length = sizeof(address);
    require(!getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &length),
            "test name failed");
    ::close(descriptor);
    return ntohs(address.sin_port);
}
} // namespace
int main(int count, char** arguments) {
    try {
        if (count == 5) {
            port = static_cast<uint16_t>(std::stoi(arguments[2]));
            SliceChannel::listen(port, SliceChannel::Protocol::TCP, receive);
            const char ready = 'R';
            (void)write(std::stoi(arguments[3]), &ready, 1);
            ::close(std::stoi(arguments[3]));
            char command;
            (void)read(std::stoi(arguments[4]), &command, 1);
            SliceChannel::close(port, SliceChannel::Protocol::TCP);
            BuffetMenu::shutdown();
            return 0;
        }
        if (ba_vk_initialize()) {
            std::puts("Vulkan contract skipped: no usable Vulkan device");
            return 77;
        }
        port = unused_port();
        int ready[2], commands[2];
        require(!pipe(ready) && !pipe(commands), "test pipes failed");
        const std::string service = std::to_string(port);
        const std::string notification = std::to_string(ready[1]);
        const std::string control = std::to_string(commands[0]);
        pid_t child = fork();
        require(child >= 0, "test fork failed");
        if (!child) {
            ::close(ready[0]);
            ::close(commands[1]);
            execl(arguments[0], arguments[0], "server", service.c_str(), notification.c_str(),
                  control.c_str(), static_cast<char*>(nullptr));
            _exit(2);
        }
        ::close(ready[1]);
        ::close(commands[0]);
        char notification_byte;
        require(read(ready[0], &notification_byte, 1) == 1, "receiver failed to start");
        ::close(ready[0]);
        try {
            for (uint32_t properties : {1u, 2u, 6u, 14u}) {
                if (!ba_vk_supported(properties)) continue;
                std::printf("Testing Vulkan memory properties %u\n", properties);
                const Placemat* placement = BuffetMenu::vulkan(properties);
                unsigned char original[1041];
                for (size_t index = 0; index < sizeof(original); ++index)
                    original[index] = index % 251;
                Slice backing(original, sizeof(original), true, placement);
                Slice view = backing.slice(3, 1025);
                const auto buffer = view.vulkan_buffer();
                require(buffer.buffer == backing.vulkan_buffer().buffer && buffer.offset == 3 &&
                            buffer.range == 1025,
                        "Vulkan view lost its original buffer offset");
                view.vulkan_sync(false);
                require(!std::memcmp(view.raw(), original + 3, 1025),
                        "Vulkan upload/readback changed bytes");
                SliceChannel::send(std::move(view), "127.0.0.1", port, SliceChannel::Protocol::TCP,
                                   response);
                require(completed.try_acquire_for(std::chrono::seconds(30)),
                        "Vulkan response did not arrive");
                require(received && received.size_bytes() == 1025, "Vulkan receive failed");
                require(received.placement() == placement,
                        "Vulkan properties changed across the network");
                require(received.vulkan_buffer().buffer != VK_NULL_HANDLE,
                        "Vulkan response has no device buffer");
                received.vulkan_sync(false);
                require(!std::memcmp(received.raw(), original + 3, 1024),
                        "Vulkan response bytes differ");
                require(received.data<unsigned char>()[1024] == (original[1027] ^ 0xA5),
                        "Vulkan reply was not uploaded");
                received.free();
            }
        } catch (...) {
            ::close(commands[1]);
            waitpid(child, nullptr, 0);
            throw;
        }
        ::close(commands[1]);
        int status;
        waitpid(child, &status, 0);
        require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "Vulkan receiver failed");
        BuffetMenu::shutdown();
        std::puts("Vulkan network contracts passed");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Vulkan test failed: %s\n", error.what());
        return 1;
    }
}
