#pragma once
/** --------------------------------------------------------------------------------------------------------- Network Interface
 * @file ba_network.h
 * @brief Defines the private C transport boundary and Slice framing operations.
 */
#include "core/ba_descriptor.h"
#include <uv.h>
#include <sodium.h>

#define BA_NET_HEADER 64u
#define BA_NET_LIMIT (64u * 1024u * 1024u)
#define BA_NET_SECURE 1u
#define BA_NET_RESPONSE 2u
#define BA_NET_REPLY 4u
#define BA_NET_NOVEL 8u
typedef void (*ba_net_callback)(void);
/** --------------------------------------------------------------------------------------------------------- Frame
 * @brief Owns one encoded network message.
 */
typedef struct ba_net_frame {
    unsigned char* data;
    size_t size;
} ba_net_frame;
/** --------------------------------------------------------------------------------------------------------- Transport Functions
 * @brief Submits channel operations to the dedicated network loop.
 */
int ba_net_listen(uint16_t port, uint8_t protocol, ba_net_callback callback);
int ba_net_close(uint16_t port, uint8_t protocol);
int ba_net_send(const ba_slice_t* slice, const char* address, uint16_t port, uint8_t protocol,
                ba_net_callback callback);
void ba_net_shutdown(void);
int ba_net_deliver(ba_net_callback callback, ba_slice_t* slice);
void ba_net_log(const char* message);
/** --------------------------------------------------------------------------------------------------------- Framing Functions
 * @brief Encodes and verifies bounded frames before publishing received Slices.
 */
int ba_net_key(unsigned char key[32]);
int ba_net_encode(const ba_slice_t* slice, unsigned flags, const unsigned char token[16],
                  const unsigned char key[32], ba_net_frame* frame);
int ba_net_frame_size(const unsigned char* header, size_t* size);
int ba_net_decode(const unsigned char* frame, size_t size, uint8_t protocol,
                  const unsigned char key[32], ba_slice_t* slice);
/** --------------------------------------------------------------------------------------------------------- Placement Resolution
 * @brief Resolves a received placement name to its registered identifier.
 */
int ba_net_placement(const char* name);
