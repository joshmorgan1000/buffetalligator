#pragma once
/** --------------------------------------------------------------------------------------------------------- Transport State
 * @file ba_transport.h
 * @brief Shares private reactor state between libuv and libfabric transports.
 */
#include "ba_network.h"

/** --------------------------------------------------------------------------------------------------------- Peer
 * @brief Holds network-loop-owned state for one listener or exchange.
 */
typedef struct ba_net_peer {
    struct ba_net_peer* next;
    uv_loop_t* loop;
    uv_tcp_t tcp;
    uv_udp_t udp;
    uv_getaddrinfo_t resolver;
    uv_connect_t connect;
    void* fabric;
    ba_net_callback callback;
    ba_net_frame outgoing;
    unsigned char header[BA_NET_HEADER];
    unsigned char* incoming;
    size_t received;
    size_t expected;
    unsigned char token[16];
    unsigned char key[32];
    uint16_t port;
    uint8_t protocol;
    unsigned references;
    unsigned writes;
    int listener;
    int server;
    int initialized;
    int resolving;
    int closing;
    int finish_writes;
} ba_net_peer;
/** --------------------------------------------------------------------------------------------------------- Peer Operations
 * @brief Keeps peer lifetime and delivery on the network loop.
 */
ba_net_peer* ba_net_peer_create(uv_loop_t* loop, uint16_t port, uint8_t protocol);
void ba_net_peer_retain(ba_net_peer* peer);
void ba_net_peer_release(ba_net_peer* peer);
void ba_net_peer_close(ba_net_peer* peer, int status);
void ba_net_received(ba_net_peer* peer, const unsigned char* data, size_t size,
                     const struct sockaddr* address);
void ba_net_stream(ba_net_peer* peer, const unsigned char* data, size_t size);
/** --------------------------------------------------------------------------------------------------------- Fabric Operations
 * @brief Adapts message endpoints and completion wait descriptors to libuv.
 */
int ba_fabric_listen(ba_net_peer* peer);
int ba_fabric_connect(ba_net_peer* peer, const char* address);
int ba_fabric_write(ba_net_peer* peer, ba_net_frame* frame);
void ba_fabric_close(ba_net_peer* peer);
