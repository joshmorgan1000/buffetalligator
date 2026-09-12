/** --------------------------------------------------------------------------------------------------------- Slice Channel
 * @file slice_channel.cpp
 * @brief Bridges the public channel operations and the C arena boundary to the private C reactor.
 */
#include <alligator.hpp>
extern "C" {
#include "network/ba_network.h"
}

namespace buffetalligator {
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
/** --------------------------------------------------------------------------------------------------------- Arena Access
 * @brief Implements the descriptor operations the C boundary needs over the C++ arena.
 */
struct SliceNetworkAccess {
    /** ------------------------------------------------------------------------------------------- Take
     * @brief Transfers a C descriptor to a callback without copying its payload or retaining its backing.
     */
    static Slice take(ba_slice_t* descriptor) noexcept {
        Slice slice;
        slice.meta_ = descriptor->meta;
        slice.cached_ = descriptor->ptr;
        descriptor->meta = BA_NULL_META;
        descriptor->ptr = nullptr;
        return slice;
    }
    /** ------------------------------------------------------------------------------------------- Claim
     * @brief Claims zeroed arena storage through the public placement registry.
     */
    static int claim(uint32_t type, size_t bytes, uint32_t flags, ba_slice_t* out) noexcept {
        try {
            const Placemat* placement = BuffetMenu::get(static_cast<uint16_t>(type));
            if (!placement) return -1;
            Slice slice(bytes, (flags & BA_CLAIM_NOVEL) != 0, placement);
            out->meta = slice.meta_;
            out->ptr = slice.cached_;
            slice.meta_ = BA_NULL_META;
            slice.cached_ = nullptr;
            return 0;
        } catch (...) {
            return -1;
        }
    }
    /** ------------------------------------------------------------------------------------------- Release
     * @brief Drops one descriptor's arena ownership and nulls it.
     */
    static void release(ba_slice_t* descriptor) noexcept {
        Slice slice;
        slice.meta_ = descriptor->meta;
        slice.cached_ = descriptor->ptr;
        slice.free();
        descriptor->meta = BA_NULL_META;
        descriptor->ptr = nullptr;
    }
    /** ------------------------------------------------------------------------------------------- Placement Of
     * @brief Resolves the owning placement identifier of a live descriptor.
     */
    static uint32_t placement_of(const ba_slice_t* descriptor) noexcept {
        if (descriptor->meta == BA_NULL_META || !descriptor->ptr) return UINT32_MAX;
        Slice view;
        view.meta_ = descriptor->meta;
        view.cached_ = descriptor->ptr;
        const Placemat* placement = view.placement();
        view.meta_ = BA_NULL_META;
        view.cached_ = nullptr;
        return placement ? placement->type() : UINT32_MAX;
    }
    /** ------------------------------------------------------------------------------------------- Novel Of
     * @brief Reports whether a live descriptor's backing is a dedicated novel buffer.
     */
    static int novel_of(const ba_slice_t* descriptor) noexcept {
        if (descriptor->meta == BA_NULL_META || !descriptor->ptr) return 0;
        Slice view;
        view.meta_ = descriptor->meta;
        view.cached_ = descriptor->ptr;
        const int novel = view.is_novel() ? 1 : 0;
        view.meta_ = BA_NULL_META;
        view.cached_ = nullptr;
        return novel;
    }
};
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
/** --------------------------------------------------------------------------------------------------------- Arena Bridge
 * @brief Implements the C boundary operations over the C++ arena registry.
 */
extern "C" {
int ba_claim(uint32_t type, size_t bytes, uint32_t flags, ba_slice_t* out) {
    return buffetalligator::SliceNetworkAccess::claim(type, bytes, flags, out);
}
void ba_release(ba_slice_t* descriptor) {
    buffetalligator::SliceNetworkAccess::release(descriptor);
}
uint32_t ba_slice_placement(const ba_slice_t* descriptor) {
    return buffetalligator::SliceNetworkAccess::placement_of(descriptor);
}
int ba_slice_is_novel(const ba_slice_t* descriptor) {
    return buffetalligator::SliceNetworkAccess::novel_of(descriptor);
}
const char* ba_placement_name(uint32_t type) {
    const size_t count = buffetalligator::BuffetMenu::count();
    if (type >= count) return nullptr;
    return buffetalligator::BuffetMenu::get(static_cast<uint16_t>(type))->name();
}
/** --------------------------------------------------------------------------------------------------------- Resolve Network Placement
 * @brief Resolves a received placement name to its registered identifier.
 */
int ba_net_placement(const char* name) {
    const auto placement = buffetalligator::BuffetMenu::get(std::string(name));
    return placement ? placement->type() : -1;
}
/** --------------------------------------------------------------------------------------------------------- Deliver
 * @brief Contains user exceptions at the C callback boundary while preserving Slice cleanup.
 */
int ba_net_deliver(ba_net_callback callback, ba_slice_t* descriptor) {
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
void ba_net_log(const char* message) {
    LOG_ERROR_STREAM << "Slice channel: " << message;
}
}
