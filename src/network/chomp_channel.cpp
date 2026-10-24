/** --------------------------------------------------------------------------------------------------------- Chomp Channel
 * @file chomp_channel.cpp
 * @brief Bridges the public channel operations and the C arena boundary to the private C reactor.
 */
#include <alligator.hpp>
#include <alligator/containers.hpp>
#include <cstring>
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
                                    ? "Encrypted Chomp channels require ALLIGATOR_NETWORK_KEY "
                                      "containing 64 hexadecimal digits"
                                    : std::string("Chomp channel: ") + uv_strerror(status);
    ALLIGATOR_THROW(message);
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Arena Access
 * @brief Implements the descriptor operations the C boundary needs over the C++ arena.
 */
struct ChompNetworkAccess {
    /** ------------------------------------------------------------------------------------------- Take
     * @brief Transfers a C descriptor to a callback without copying its payload or retaining its backing.
     */
    static Chomp take(ba_chomp_t* descriptor) noexcept {
        Chomp chomp;
        if (descriptor->id != BA_NULL_ID) {
            chomp.id_ = static_cast<uint32_t>(descriptor->id);
        }
        descriptor->id = BA_NULL_ID;
        return chomp;
    }
    /** ------------------------------------------------------------------------------------------- Claim
     * @brief Claims zeroed arena storage through the public placement registry.
     */
    static int claim(uint32_t type, size_t bytes, uint32_t flags, ba_chomp_t* out) noexcept {
        try {
            const BuffetDescriptor* placement = BuffetDescriptors::get(type);
            if (!placement) return -1;
            Chomp chomp(bytes, (flags & BA_CLAIM_NOVEL) != 0, placement);
            out->id = chomp.pool_index();
            chomp.id_ = 0xFFFFFFFFu;
            return 0;
        } catch (...) {
            return -1;
        }
    }
    /** ------------------------------------------------------------------------------------------- Release
     * @brief Drops one descriptor's arena ownership and nulls it.
     */
    static void release(ba_chomp_t* descriptor) noexcept {
        if (descriptor->id == BA_NULL_ID) return;
        Chomp chomp;
        chomp.id_ = static_cast<uint32_t>(descriptor->id);
        descriptor->id = BA_NULL_ID;
        chomp.free();
    }
    /** ------------------------------------------------------------------------------------------- Placement Of
     * @brief Resolves the owning placement identifier of a live descriptor.
     */
    static uint32_t placement_of(const ba_chomp_t* descriptor) noexcept {
        if (descriptor->id == BA_NULL_ID) return UINT32_MAX;
        Chomp view;
        view.id_ = static_cast<uint32_t>(descriptor->id);
        const BuffetDescriptor* placement = view.placement();
        view.id_ = 0xFFFFFFFFu;
        return placement ? uint32_t(*placement->type_idx_getter()) : UINT32_MAX;
    }
    /** ------------------------------------------------------------------------------------------- Novel Of
     * @brief Reports whether a live descriptor's backing is a dedicated novel buffer.
     */
    static int novel_of(const ba_chomp_t* descriptor) noexcept {
        if (descriptor->id == BA_NULL_ID) return 0;
        Chomp view;
        view.id_ = static_cast<uint32_t>(descriptor->id);
        const int novel = view.is_novel() ? 1 : 0;
        view.id_ = 0xFFFFFFFFu;
        return novel;
    }
    /** ------------------------------------------------------------------------------------------- Size Of
     * @brief Resolves a live descriptor's byte size without taking its ownership.
     */
    static size_t size_of(const ba_chomp_t* descriptor) noexcept {
        if (descriptor->id == BA_NULL_ID) return 0;
        Chomp view;
        view.id_ = static_cast<uint32_t>(descriptor->id);
        const size_t bytes = view.size_bytes();
        view.id_ = 0xFFFFFFFFu;
        return bytes;
    }
    /** ------------------------------------------------------------------------------------------- Ptr Of
     * @brief Resolves a live descriptor's host pointer without taking its ownership.
     */
    static void* ptr_of(const ba_chomp_t* descriptor) noexcept {
        if (descriptor->id == BA_NULL_ID) return nullptr;
        Chomp view;
        view.id_ = static_cast<uint32_t>(descriptor->id);
        void* ptr = view.raw();
        view.id_ = 0xFFFFFFFFu;
        return ptr;
    }
};
/** --------------------------------------------------------------------------------------------------------- Listen
 * @brief Binds the requested listener before returning to the caller.
 */
void ChompChannel::listen(uint16_t port, Protocol protocol, void (*recv)(Chomp)) {
    (void)Chomp::default_placement();
    check_network(ba_net_listen(port, static_cast<uint8_t>(protocol),
                                reinterpret_cast<ba_net_callback>(recv), 30000));
}
/** --------------------------------------------------------------------------------------------------------- Close
 * @brief Cancels listener and exchange activity for the requested channel.
 */
void ChompChannel::close(uint16_t port, Protocol protocol) {
    check_network(ba_net_close(port, static_cast<uint8_t>(protocol)));
}
/** --------------------------------------------------------------------------------------------------------- Send
 * @brief Keeps the source alive until the reactor has captured its bytes.
 */
void ChompChannel::send(Chomp chomp, std::string address, uint16_t port, Protocol protocol,
                        void (*resp)(Chomp)) {
    if (address.find('\0') != std::string::npos)
        ALLIGATOR_THROW("Chomp address contains a null byte");
    check_network(ba_net_send(reinterpret_cast<const ba_chomp_t*>(&chomp), address.c_str(), port,
                              static_cast<uint8_t>(protocol),
                              reinterpret_cast<ba_net_callback>(resp), 30000));
}
} // namespace buffetalligator
/** --------------------------------------------------------------------------------------------------------- Arena Bridge
 * @brief Implements the C boundary operations over the C++ arena registry.
 */
extern "C" {
int ba_claim(uint32_t type, size_t bytes, uint32_t flags, ba_chomp_t* out) {
    return buffetalligator::ChompNetworkAccess::claim(type, bytes, flags, out);
}
void ba_release(ba_chomp_t* descriptor) {
    buffetalligator::ChompNetworkAccess::release(descriptor);
}
uint32_t ba_chomp_placement(const ba_chomp_t* descriptor) {
    return buffetalligator::ChompNetworkAccess::placement_of(descriptor);
}
int ba_chomp_is_novel(const ba_chomp_t* descriptor) {
    return buffetalligator::ChompNetworkAccess::novel_of(descriptor);
}
size_t ba_chomp_size(const ba_chomp_t* descriptor) {
    return buffetalligator::ChompNetworkAccess::size_of(descriptor);
}
void* ba_chomp_ptr(const ba_chomp_t* descriptor) {
    return buffetalligator::ChompNetworkAccess::ptr_of(descriptor);
}
const char* ba_placement_name(uint32_t type) {
    const buffetalligator::BuffetDescriptor* placement = buffetalligator::BuffetDescriptors::get(type);
    return placement ? placement->type_name_fn() : nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Resolve Network Placement
 * @brief Resolves a received placement name to its registered identifier.
 */
int ba_net_placement(const char* name) {
    for (size_t type = 0;; ++type) {
        const buffetalligator::BuffetDescriptor* placement = buffetalligator::BuffetDescriptors::get(type);
        if (!placement) return -1;
        if (std::strcmp(placement->type_name_fn(), name) == 0) return static_cast<int>(type);
    }
}
/** --------------------------------------------------------------------------------------------------------- Deliver
 * @brief Contains user exceptions at the C callback boundary while preserving Chomp cleanup.
 */
int ba_net_deliver(ba_net_callback callback, ba_chomp_t* descriptor) {
    try {
        reinterpret_cast<void (*)(buffetalligator::Chomp)>(callback)(
            buffetalligator::ChompNetworkAccess::take(descriptor));
        return 0;
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Chomp callback failed: " << error.what();
    } catch (...) {
        LOG_ERROR_STREAM << "Chomp callback failed with an unknown exception";
    }
    return UV_EIO;
}
/** --------------------------------------------------------------------------------------------------------- Log
 * @brief Routes private C transport diagnostics through the application's logger.
 */
void ba_net_log(const char* message) {
    LOG_ERROR_STREAM << "Chomp channel: " << message;
}
}
