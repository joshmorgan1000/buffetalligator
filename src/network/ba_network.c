/** --------------------------------------------------------------------------------------------------------- Network Reactor
 * @file ba_network.c
 * @brief Runs Slice exchanges on one libuv loop with a command handoff from application threads.
 */
#include "ba_transport.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

/** --------------------------------------------------------------------------------------------------------- Command
 * @brief Publishes one operation and its completion through the reactor mailbox.
 */
typedef struct ba_net_command {
    struct ba_net_command* next;
    int operation;
    const char* address;
    uint16_t port;
    uint8_t protocol;
    ba_net_callback callback;
    const ba_slice_t* slice;
    uv_sem_t completion;
    int status;
} ba_net_command;
/** --------------------------------------------------------------------------------------------------------- Reply Context
 * @brief Identifies the sender only while its receive callback is executing.
 */
typedef struct ba_net_reply {
    ba_net_peer* peer;
    const struct sockaddr* address;
    const unsigned char* token;
    unsigned flags;
    int replied;
} ba_net_reply;
/** --------------------------------------------------------------------------------------------------------- Write
 * @brief Retains encoded bytes until libuv completes their transmission.
 */
typedef struct ba_net_write {
    uv_write_t tcp;
    uv_udp_send_t udp;
    ba_net_peer* peer;
    ba_net_frame frame;
} ba_net_write;
static uv_once_t ba_network_once = UV_ONCE_INIT;
static uv_loop_t ba_network_loop;
static uv_thread_t ba_network_thread;
static uv_async_t ba_network_mailbox;
static uv_mutex_t ba_network_mutex;
static ba_net_command* ba_network_head;
static ba_net_command* ba_network_tail;
static ba_net_peer* ba_network_peers;
static ba_net_reply* ba_network_reply;
static int ba_network_status;
static _Atomic int ba_network_stopped;
static _Atomic int ba_network_started;
static void ba_network_execute(ba_net_command* command);
/** --------------------------------------------------------------------------------------------------------- Retain
 * @brief Keeps a peer alive while a libuv or fabric operation references it.
 */
void ba_net_peer_retain(ba_net_peer* peer) { ++peer->references; }
/** --------------------------------------------------------------------------------------------------------- Release
 * @brief Frees a peer after its final asynchronous operation has retired.
 */
void ba_net_peer_release(ba_net_peer* peer) {
    if (--peer->references) return;
    free(peer->incoming);
    free(peer->outgoing.data);
    sodium_memzero(peer->key, sizeof(peer->key));
    free(peer);
}
/** --------------------------------------------------------------------------------------------------------- Create Peer
 * @brief Adds one exchange to the reactor-owned live list.
 */
ba_net_peer* ba_net_peer_create(uv_loop_t* loop, uint16_t port, uint8_t protocol) {
    ba_net_peer* peer = calloc(1, sizeof(*peer));
    if (!peer) return NULL;
    peer->loop = loop;
    peer->port = port;
    peer->protocol = protocol;
    peer->references = 1;
    peer->next = ba_network_peers;
    ba_network_peers = peer;
    return peer;
}
/** --------------------------------------------------------------------------------------------------------- Handle Closed
 * @brief Releases the reference held by a libuv handle.
 */
static void ba_network_closed(uv_handle_t* handle) { ba_net_peer_release(handle->data); }
/** --------------------------------------------------------------------------------------------------------- Close Peer
 * @brief Cancels operations and reports each pending response failure exactly once.
 */
void ba_net_peer_close(ba_net_peer* peer, int status) {
    if (peer->closing) return;
    peer->closing = 1;
    ba_net_peer** position = &ba_network_peers;
    while (*position != peer) position = &(*position)->next;
    *position = peer->next;
    if (peer->resolving) uv_cancel((uv_req_t*)&peer->resolver);
    if (peer->initialized) {
        uv_handle_t* handle =
            (peer->protocol & 127) == 1 ? (uv_handle_t*)&peer->udp : (uv_handle_t*)&peer->tcp;
        uv_close(handle, ba_network_closed);
    }
    if (peer->fabric) ba_fabric_close(peer);
    if (!peer->server && peer->callback) {
        ba_net_callback callback = peer->callback;
        peer->callback = NULL;
        ba_slice_t empty = {BA_NULL_META, NULL};
        ba_net_deliver(callback, &empty);
    }
    if (status && status != UV_ECANCELED && status != UV_EOF) ba_net_log(uv_strerror(status));
    ba_net_peer_release(peer);
}
/** --------------------------------------------------------------------------------------------------------- Write Finished
 * @brief Retires encoded output and closes completed one-shot exchanges.
 */
static void ba_network_write_finished(ba_net_write* write, int status) {
    ba_net_peer* peer = write->peer;
    free(write->frame.data);
    free(write);
    --peer->writes;
    if (status) ba_net_peer_close(peer, status);
    else if (peer->finish_writes && !peer->writes) ba_net_peer_close(peer, 0);
    ba_net_peer_release(peer);
}
/** --------------------------------------------------------------------------------------------------------- TCP Written
 * @brief Receives libuv stream write completion.
 */
static void ba_network_tcp_written(uv_write_t* request, int status) {
    ba_network_write_finished(request->data, status);
}
/** --------------------------------------------------------------------------------------------------------- UDP Written
 * @brief Receives libuv datagram write completion.
 */
static void ba_network_udp_written(uv_udp_send_t* request, int status) {
    ba_network_write_finished(request->data, status);
}
/** --------------------------------------------------------------------------------------------------------- Write Frame
 * @brief Moves a frame into the selected transport without exposing its state publicly.
 */
static int ba_network_write(ba_net_peer* peer, ba_net_frame* frame,
                            const struct sockaddr* address) {
    if ((peer->protocol & 127) == 2) return ba_fabric_write(peer, frame);
    ba_net_write* write = calloc(1, sizeof(*write));
    if (!write) return UV_ENOMEM;
    write->peer = peer;
    write->frame = *frame;
    uv_buf_t buffer = uv_buf_init((char*)frame->data, (unsigned)frame->size);
    int status;
    if ((peer->protocol & 127) == 1) {
        write->udp.data = write;
        status = uv_udp_send(&write->udp, &peer->udp, &buffer, 1, address, ba_network_udp_written);
    } else {
        write->tcp.data = write;
        status =
            uv_write(&write->tcp, (uv_stream_t*)&peer->tcp, &buffer, 1, ba_network_tcp_written);
    }
    if (status) {
        free(write);
        return status;
    }
    *frame = (ba_net_frame){0};
    ++peer->writes;
    ba_net_peer_retain(peer);
    return 0;
}
/** --------------------------------------------------------------------------------------------------------- Receive Frame
 * @brief Publishes a complete Slice and scopes implicit replies to its receive callback.
 */
void ba_net_received(ba_net_peer* peer, const unsigned char* data, size_t size,
                     const struct sockaddr* address) {
    ba_slice_t slice;
    const int status = ba_net_decode(data, size, peer->protocol, peer->key, &slice);
    if (status) {
        if (peer->listener) ba_net_log(uv_strerror(status));
        else ba_net_peer_close(peer, status);
        return;
    }
    const int response = !!(data[5] & BA_NET_RESPONSE);
    if (response == peer->server || (!peer->server && sodium_memcmp(data + 16, peer->token, 16))) {
        ba_release(&slice);
        if (!peer->listener) ba_net_peer_close(peer, UV_EPROTO);
        return;
    }
    ba_net_peer_retain(peer);
    if (!peer->server) {
        ba_net_callback callback = peer->callback;
        peer->callback = NULL;
        if (callback) ba_net_deliver(callback, &slice);
        else ba_release(&slice);
        ba_net_peer_close(peer, 0);
    } else {
        ba_net_reply reply = {peer, address, data + 16, data[5], 0};
        ba_net_reply* previous = ba_network_reply;
        ba_network_reply = &reply;
        ba_net_deliver(peer->callback, &slice);
        ba_network_reply = previous;
        if (!peer->listener && !peer->closing) {
            peer->finish_writes = 1;
            if (!peer->writes) ba_net_peer_close(peer, 0);
        }
    }
    ba_net_peer_release(peer);
}
/** --------------------------------------------------------------------------------------------------------- Receive Stream
 * @brief Reassembles split headers and payloads without assuming TCP read boundaries.
 */
void ba_net_stream(ba_net_peer* peer, const unsigned char* data, size_t size) {
    while (size && !peer->closing) {
        if (peer->received < BA_NET_HEADER) {
            size_t bytes = BA_NET_HEADER - peer->received;
            if (bytes > size) bytes = size;
            memcpy(peer->header + peer->received, data, bytes);
            peer->received += bytes;
            data += bytes;
            size -= bytes;
            if (peer->received < BA_NET_HEADER) return;
            int status = ba_net_frame_size(peer->header, &peer->expected);
            if (status) {
                ba_net_peer_close(peer, status);
                return;
            }
            peer->incoming = malloc(peer->expected);
            if (!peer->incoming) {
                ba_net_peer_close(peer, UV_ENOMEM);
                return;
            }
            memcpy(peer->incoming, peer->header, BA_NET_HEADER);
        }
        size_t bytes = peer->expected - peer->received;
        if (bytes > size) bytes = size;
        memcpy(peer->incoming + peer->received, data, bytes);
        peer->received += bytes;
        data += bytes;
        size -= bytes;
        if (peer->received == peer->expected) {
            unsigned char* complete = peer->incoming;
            peer->incoming = NULL;
            ba_net_received(peer, complete, peer->expected, NULL);
            free(complete);
            return;
        }
    }
}
/** --------------------------------------------------------------------------------------------------------- Read Storage
 * @brief Supplies bounded scratch storage for each libuv read.
 */
static void ba_network_allocate(uv_handle_t* handle, size_t suggested, uv_buf_t* buffer) {
    (void)handle;
    (void)suggested;
    buffer->base = malloc(65536);
    buffer->len = buffer->base ? 65536 : 0;
}
/** --------------------------------------------------------------------------------------------------------- TCP Read
 * @brief Advances stream framing or closes a failed connection.
 */
static void ba_network_tcp_read(uv_stream_t* stream, ssize_t bytes, const uv_buf_t* buffer) {
    ba_net_peer* peer = stream->data;
    if (bytes > 0) ba_net_stream(peer, (unsigned char*)buffer->base, (size_t)bytes);
    else if (bytes < 0) ba_net_peer_close(peer, (int)bytes);
    free(buffer->base);
}
/** --------------------------------------------------------------------------------------------------------- UDP Read
 * @brief Delivers only complete datagrams and rejects truncated input.
 */
static void ba_network_udp_read(uv_udp_t* socket, ssize_t bytes, const uv_buf_t* buffer,
                                const struct sockaddr* address, unsigned flags) {
    ba_net_peer* peer = socket->data;
    if (bytes > 0 && !(flags & UV_UDP_PARTIAL))
        ba_net_received(peer, (unsigned char*)buffer->base, (size_t)bytes, address);
    else if (bytes < 0) ba_net_peer_close(peer, (int)bytes);
    free(buffer->base);
}
/** --------------------------------------------------------------------------------------------------------- Initialize Handle
 * @brief Initializes the appropriate libuv handle and its lifetime reference.
 */
static int ba_network_handle(ba_net_peer* peer) {
    int status;
    if ((peer->protocol & 127) == 1) {
        status = uv_udp_init(peer->loop, &peer->udp);
        peer->udp.data = peer;
    } else {
        status = uv_tcp_init(peer->loop, &peer->tcp);
        peer->tcp.data = peer;
    }
    if (!status) {
        peer->initialized = 1;
        ba_net_peer_retain(peer);
    }
    return status;
}
/** --------------------------------------------------------------------------------------------------------- Datagram Buffers
 * @brief Allows full-size UDP datagrams independently of the operating system's smaller defaults.
 */
static int ba_network_datagram_buffers(ba_net_peer* peer) {
    int bytes = 1024 * 1024;
    int status = uv_send_buffer_size((uv_handle_t*)&peer->udp, &bytes);
    bytes = 1024 * 1024;
    if (!status) status = uv_recv_buffer_size((uv_handle_t*)&peer->udp, &bytes);
    return status;
}
/** --------------------------------------------------------------------------------------------------------- Accept
 * @brief Accepts a TCP exchange using the listener's callback and encryption policy.
 */
static void ba_network_accept(uv_stream_t* server, int status) {
    ba_net_peer* listener = server->data;
    if (status) {
        ba_net_log(uv_strerror(status));
        return;
    }
    ba_net_peer* peer = ba_net_peer_create(listener->loop, listener->port, listener->protocol);
    if (!peer) {
        ba_net_log("Cannot allocate incoming Slice exchange");
        return;
    }
    peer->server = 1;
    peer->callback = listener->callback;
    memcpy(peer->key, listener->key, 32);
    status = ba_network_handle(peer);
    if (!status) status = uv_accept(server, (uv_stream_t*)&peer->tcp);
    if (!status)
        status = uv_read_start((uv_stream_t*)&peer->tcp, ba_network_allocate, ba_network_tcp_read);
    if (status) ba_net_peer_close(peer, status);
}
/** --------------------------------------------------------------------------------------------------------- Connected
 * @brief Sends the queued frame after a successful TCP connection.
 */
static void ba_network_connected(uv_connect_t* request, int status) {
    ba_net_peer* peer = request->data;
    if (!status && !peer->closing) {
        status = uv_read_start((uv_stream_t*)&peer->tcp, ba_network_allocate, ba_network_tcp_read);
        if (!status) status = ba_network_write(peer, &peer->outgoing, NULL);
    }
    if (status) ba_net_peer_close(peer, status);
    ba_net_peer_release(peer);
}
/** --------------------------------------------------------------------------------------------------------- Resolved
 * @brief Opens a TCP or connected UDP exchange using the resolved destination.
 */
static void ba_network_resolved(uv_getaddrinfo_t* request, int status,
                                struct addrinfo* addresses) {
    ba_net_peer* peer = request->data;
    peer->resolving = 0;
    if (!status && !peer->closing) {
        status = ba_network_handle(peer);
        if (!status && (peer->protocol & 127) == 1) {
            status = uv_udp_connect(&peer->udp, addresses->ai_addr);
            if (!status) status = ba_network_datagram_buffers(peer);
            if (!status)
                status = uv_udp_recv_start(&peer->udp, ba_network_allocate, ba_network_udp_read);
            if (!status) status = ba_network_write(peer, &peer->outgoing, NULL);
        } else if (!status) {
            peer->connect.data = peer;
            status = uv_tcp_connect(&peer->connect, &peer->tcp, addresses->ai_addr,
                                    ba_network_connected);
            if (!status) ba_net_peer_retain(peer);
        }
    }
    if (addresses) uv_freeaddrinfo(addresses);
    if (status) ba_net_peer_close(peer, status);
    ba_net_peer_release(peer);
}
/** --------------------------------------------------------------------------------------------------------- Reply
 * @brief Uses the callback-scoped peer for one response without another public operation.
 */
static int ba_network_respond(ba_net_command* command) {
    ba_net_reply* reply = ba_network_reply;
    if (!reply || reply->peer->closing || reply->replied || command->slice->meta == BA_NULL_META ||
        command->callback || command->port != reply->peer->port ||
        command->protocol != reply->peer->protocol)
        return UV_EINVAL;
    if (!(reply->flags & BA_NET_REPLY)) {
        reply->replied = 1;
        return 0;
    }
    ba_net_frame frame = {0};
    int status = ba_net_encode(command->slice,
                               BA_NET_RESPONSE | ((command->protocol & 128) ? BA_NET_SECURE : 0),
                               reply->token, reply->peer->key, &frame);
    if (!status && (command->protocol & 127) == 1 && frame.size > 65507) status = UV_EMSGSIZE;
    if (!status) status = ba_network_write(reply->peer, &frame, reply->address);
    free(frame.data);
    if (!status) reply->replied = 1;
    return status;
}
/** --------------------------------------------------------------------------------------------------------- Execute
 * @brief Performs each mutation under the network thread's exclusive ownership.
 */
static void ba_network_execute(ba_net_command* command) {
    if (command->operation == 3) {
        while (ba_network_peers) ba_net_peer_close(ba_network_peers, UV_ECANCELED);
        uv_close((uv_handle_t*)&ba_network_mailbox, NULL);
        return;
    }
    if (!command->port || (command->protocol & 127) > 2) {
        command->status = UV_EINVAL;
        return;
    }
    if (command->operation == 1) {
        for (;;) {
            ba_net_peer* peer = ba_network_peers;
            while (peer && (peer->port != command->port || peer->protocol != command->protocol))
                peer = peer->next;
            if (!peer) break;
            ba_net_peer_close(peer, UV_ECANCELED);
        }
        return;
    }
    if (command->operation == 2 && !command->address[0]) {
        command->status = ba_network_respond(command);
        return;
    }
    if (command->operation == 0) {
        if (!command->callback) {
            command->status = UV_EINVAL;
            return;
        }
        for (ba_net_peer* entry = ba_network_peers; entry; entry = entry->next) {
            if (entry->listener && entry->port == command->port &&
                (entry->protocol & 127) == (command->protocol & 127)) {
                command->status = UV_EADDRINUSE;
                return;
            }
        }
    }
    ba_net_peer* peer = ba_net_peer_create(&ba_network_loop, command->port, command->protocol);
    if (!peer) {
        command->status = UV_ENOMEM;
        return;
    }
    peer->server = peer->listener = command->operation == 0;
    int status = (command->protocol & 128) ? ba_net_key(peer->key) : 0;
    if (!status && command->operation == 0) {
        peer->callback = command->callback;
        if ((peer->protocol & 127) == 2) status = ba_fabric_listen(peer);
        else {
            struct sockaddr_in6 address;
            status = uv_ip6_addr("::", peer->port, &address);
            if (!status) status = ba_network_handle(peer);
            if (!status && (peer->protocol & 127) == 1) {
                status = uv_udp_bind(&peer->udp, (struct sockaddr*)&address, 0);
                if (!status) status = ba_network_datagram_buffers(peer);
                if (!status)
                    status =
                        uv_udp_recv_start(&peer->udp, ba_network_allocate, ba_network_udp_read);
            } else if (!status) {
                status = uv_tcp_bind(&peer->tcp, (struct sockaddr*)&address, 0);
                if (!status) status = uv_listen((uv_stream_t*)&peer->tcp, 128, ba_network_accept);
            }
        }
    } else if (!status) {
        randombytes_buf(peer->token, 16);
        unsigned flags = (peer->protocol & 128) ? BA_NET_SECURE : 0;
        if (command->callback) flags |= BA_NET_REPLY;
        status = ba_net_encode(command->slice, flags, peer->token, peer->key, &peer->outgoing);
        peer->finish_writes = !command->callback;
        if (!status && (peer->protocol & 127) == 1 && peer->outgoing.size > 65507)
            status = UV_EMSGSIZE;
        if (!status && (peer->protocol & 127) == 2)
            status = ba_fabric_connect(peer, command->address);
        else if (!status) {
            char service[6];
            snprintf(service, sizeof(service), "%u", peer->port);
            struct addrinfo hints = {0};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = (peer->protocol & 127) == 1 ? SOCK_DGRAM : SOCK_STREAM;
            peer->resolver.data = peer;
            status = uv_getaddrinfo(peer->loop, &peer->resolver, ba_network_resolved,
                                    command->address, service, &hints);
            if (!status) {
                peer->resolving = 1;
                ba_net_peer_retain(peer);
            }
        }
        if (!status) peer->callback = command->callback;
    }
    if (status) ba_net_peer_close(peer, 0);
    command->status = status;
}
/** --------------------------------------------------------------------------------------------------------- Mailbox
 * @brief Detaches published commands before executing callbacks or I/O without the producer lock.
 */
static void ba_network_commands(uv_async_t* mailbox) {
    (void)mailbox;
    uv_mutex_lock(&ba_network_mutex);
    ba_net_command* command = ba_network_head;
    ba_network_head = ba_network_tail = NULL;
    uv_mutex_unlock(&ba_network_mutex);
    while (command) {
        ba_net_command* next = command->next;
        ba_network_execute(command);
        uv_sem_post(&command->completion);
        command = next;
    }
}
/** --------------------------------------------------------------------------------------------------------- Reactor
 * @brief Owns transport resources until shutdown drains every close callback.
 */
static void ba_network_run(void* unused) {
    (void)unused;
    uv_run(&ba_network_loop, UV_RUN_DEFAULT);
    uv_loop_close(&ba_network_loop);
}
/** --------------------------------------------------------------------------------------------------------- Initialize
 * @brief Starts the single reactor and process-exit cleanup exactly once.
 */
static void ba_network_initialize(void) {
    if (sodium_init() < 0) {
        ba_network_status = UV_EIO;
        return;
    }
    ba_network_status = uv_mutex_init(&ba_network_mutex);
    if (ba_network_status) return;
    ba_network_status = uv_loop_init(&ba_network_loop);
    if (ba_network_status) {
        uv_mutex_destroy(&ba_network_mutex);
        return;
    }
    ba_network_status = uv_async_init(&ba_network_loop, &ba_network_mailbox, ba_network_commands);
    if (ba_network_status) {
        uv_loop_close(&ba_network_loop);
        uv_mutex_destroy(&ba_network_mutex);
        return;
    }
    ba_network_status = uv_thread_create(&ba_network_thread, ba_network_run, NULL);
    if (ba_network_status) {
        uv_close((uv_handle_t*)&ba_network_mailbox, NULL);
        uv_run(&ba_network_loop, UV_RUN_DEFAULT);
        uv_loop_close(&ba_network_loop);
        uv_mutex_destroy(&ba_network_mutex);
        return;
    }
    atomic_store_explicit(&ba_network_started, 1, memory_order_release);
    atexit(ba_net_shutdown);
}
/** --------------------------------------------------------------------------------------------------------- Submit
 * @brief Publishes a stack command until the reactor signals that its inputs have been consumed.
 */
static int ba_network_submit(ba_net_command* command) {
    uv_once(&ba_network_once, ba_network_initialize);
    if (ba_network_status) return ba_network_status;
    if (atomic_load_explicit(&ba_network_stopped, memory_order_acquire)) return UV_ECANCELED;
    uv_thread_t caller = uv_thread_self();
    if (uv_thread_equal(&caller, &ba_network_thread)) {
        ba_network_execute(command);
        return command->status;
    }
    int status = uv_sem_init(&command->completion, 0);
    if (status) return status;
    uv_mutex_lock(&ba_network_mutex);
    if (atomic_load_explicit(&ba_network_stopped, memory_order_acquire)) {
        uv_mutex_unlock(&ba_network_mutex);
        uv_sem_destroy(&command->completion);
        return UV_ECANCELED;
    }
    if (command->operation == 3)
        atomic_store_explicit(&ba_network_stopped, 1, memory_order_release);
    if (ba_network_tail) ba_network_tail->next = command;
    else ba_network_head = command;
    ba_network_tail = command;
    uv_async_send(&ba_network_mailbox);
    uv_mutex_unlock(&ba_network_mutex);
    uv_sem_wait(&command->completion);
    uv_sem_destroy(&command->completion);
    return command->status;
}
/** --------------------------------------------------------------------------------------------------------- Listen
 * @brief Returns only after the listener has bound or produced a synchronous error.
 */
int ba_net_listen(uint16_t port, uint8_t protocol, ba_net_callback callback) {
    ba_net_command command = {.port = port, .protocol = protocol, .callback = callback};
    return ba_network_submit(&command);
}
/** --------------------------------------------------------------------------------------------------------- Close
 * @brief Prevents further receive callbacks for the selected port and protocol.
 */
int ba_net_close(uint16_t port, uint8_t protocol) {
    ba_net_command command = {.operation = 1, .port = port, .protocol = protocol};
    return ba_network_submit(&command);
}
/** --------------------------------------------------------------------------------------------------------- Send
 * @brief Snapshots the Slice before returning and completes any response asynchronously.
 */
int ba_net_send(const ba_slice_t* slice, const char* address, uint16_t port, uint8_t protocol,
                ba_net_callback callback) {
    ba_net_command command = {.operation = 2,
                              .address = address,
                              .port = port,
                              .protocol = protocol,
                              .callback = callback,
                              .slice = slice};
    return ba_network_submit(&command);
}
/** --------------------------------------------------------------------------------------------------------- Shutdown
 * @brief Joins the reactor before the allocator is shut down at application quiescence.
 */
void ba_net_shutdown(void) {
    if (!atomic_load_explicit(&ba_network_started, memory_order_acquire)) return;
    ba_net_command command = {.operation = 3};
    if (!ba_network_submit(&command)) uv_thread_join(&ba_network_thread);
}
