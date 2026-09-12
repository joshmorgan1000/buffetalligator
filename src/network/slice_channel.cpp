/** --------------------------------------------------------------------------------------------------------- Slice Channel
 * @file slice_channel.cpp
 * @brief Bridges the three public channel operations to the private C reactor.
 */
#include <alligator.hpp>
extern "C" {
#include "network/ba_network.h"
}

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Slice Access
 * @brief Transfers a C descriptor to a callback without copying its payload or retaining its backing.
 */
struct SliceNetworkAccess {
    static Slice take(ba_slice_t* descriptor) {
        Slice slice;
        slice.meta_ = descriptor->meta;
        slice.cached_ = descriptor->ptr;
        *descriptor = {BA_NULL_META, nullptr};
        return slice;
    }
};
namespace {
/** --------------------------------------------------------------------------------------------------------- Check
 * @brief Converts synchronous setup failures into the library's existing exception type.
 */
void check_network(int status) {
    if (!status) return;
    const std::string message = status == UV_EACCES
                                    ? "Encrypted Slice channels require ALLIGATOR_NETWORK_KEY "
                                      "containing 64 hexadecimal digits"
                                    : std::string("Slice channel: ") + uv_strerror(status);
    ALLIGATOR_THROW(message);
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Listen
 * @brief Binds the requested listener before returning to the caller.
 */
void SliceChannel::listen(uint16_t port, Protocol protocol, void (*recv)(Slice)) {
    (void)Slice::default_placement();
    check_network(ba_net_listen(port, static_cast<uint8_t>(protocol),
                                reinterpret_cast<ba_net_callback>(recv)));
}
/** --------------------------------------------------------------------------------------------------------- Close
 * @brief Cancels listener and exchange activity for the requested channel.
 */
void SliceChannel::close(uint16_t port, Protocol protocol) {
    check_network(ba_net_close(port, static_cast<uint8_t>(protocol)));
}
/** --------------------------------------------------------------------------------------------------------- Send
 * @brief Keeps the source alive until the reactor has captured its bytes.
 */
void SliceChannel::send(Slice slice, std::string address, uint16_t port, Protocol protocol,
                        void (*resp)(Slice)) {
    if (address.find('\0') != std::string::npos)
        ALLIGATOR_THROW("Slice address contains a null byte");
    check_network(ba_net_send(reinterpret_cast<const ba_slice_t*>(&slice), address.c_str(), port,
                              static_cast<uint8_t>(protocol),
                              reinterpret_cast<ba_net_callback>(resp)));
}
} // namespace buffetalligator
/** --------------------------------------------------------------------------------------------------------- Deliver
 * @brief Contains user exceptions at the C callback boundary while preserving Slice cleanup.
 */
extern "C" int ba_net_deliver(ba_net_callback callback, ba_slice_t* descriptor) {
    try {
        reinterpret_cast<void (*)(buffetalligator::Slice)>(callback)(
            buffetalligator::SliceNetworkAccess::take(descriptor));
        return 0;
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Slice callback failed: " << error.what();
    } catch (...) {
        LOG_ERROR_STREAM << "Slice callback failed with an unknown exception";
    }
    return UV_EIO;
}
/** --------------------------------------------------------------------------------------------------------- Log
 * @brief Routes private C transport diagnostics through the application's logger.
 */
extern "C" void ba_net_log(const char* message) {
    LOG_ERROR_STREAM << "Slice channel: " << message;
}
