/** --------------------------------------------------------------------------------------------------------- Fabric Transport
 * @file ba_fabric.c
 * @brief Integrates libfabric message endpoints and completion wait descriptors with the network loop.
 */
#include "ba_transport.h"
#include <rdma/fabric.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_eq.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/** --------------------------------------------------------------------------------------------------------- Fabric Root
 * @brief Shares a fabric across a passive endpoint and its accepted connections.
 */
typedef struct ba_fabric_root {
    struct fid_fabric* handle;
    unsigned references;
} ba_fabric_root;
/** --------------------------------------------------------------------------------------------------------- Fabric State
 * @brief Owns one endpoint and its event-driven transmit and receive resources.
 */
typedef struct ba_fabric_state {
    ba_net_peer* peer;
    ba_fabric_root* root;
    struct fid_domain* domain;
    struct fid_pep* passive;
    struct fid_ep* endpoint;
    struct fid_eq* events;
    struct fid_cq* completions;
    struct fid_mr* receive_memory;
    struct fid_mr* transmit_memory;
    struct fi_context receive_context;
    struct fi_context transmit_context;
    uv_poll_t event_poll;
    uv_poll_t completion_poll;
    uv_prepare_t prepare;
    uv_idle_t progress;
    unsigned handles;
    int event_initialized;
    int completion_initialized;
    int prepare_initialized;
    int progress_initialized;
    int transmitting;
    int closing;
    size_t chunk;
    size_t sent;
    size_t pending;
    ba_net_frame transmit;
    unsigned char receive[65536];
} ba_fabric_state;
static void ba_fabric_drain_events(ba_fabric_state* state);
static void ba_fabric_drain_completions(ba_fabric_state* state);
/** --------------------------------------------------------------------------------------------------------- Error
 * @brief Logs the provider's diagnostic without converting RDMA into another protocol.
 */
static int ba_fabric_error(int status) {
    ba_net_log(fi_strerror(-status));
    return UV_EIO;
}
/** --------------------------------------------------------------------------------------------------------- Destroy
 * @brief Releases fabric resources only after all reactor callbacks have retired.
 */
static void ba_fabric_destroy(ba_fabric_state* state) {
    if (state->endpoint) fi_close(&state->endpoint->fid);
    if (state->passive) fi_close(&state->passive->fid);
    if (state->receive_memory) fi_close(&state->receive_memory->fid);
    if (state->transmit_memory) fi_close(&state->transmit_memory->fid);
    if (state->completions) fi_close(&state->completions->fid);
    if (state->events) fi_close(&state->events->fid);
    if (state->domain) fi_close(&state->domain->fid);
    if (state->root && !--state->root->references) {
        fi_close(&state->root->handle->fid);
        free(state->root);
    }
    free(state->transmit.data);
    ba_net_peer* peer = state->peer;
    peer->fabric = NULL;
    free(state);
    ba_net_peer_release(peer);
}
/** --------------------------------------------------------------------------------------------------------- Poll Closed
 * @brief Defers fabric destruction until libuv no longer references provider descriptors.
 */
static void ba_fabric_poll_closed(uv_handle_t* handle) {
    ba_fabric_state* state = handle->data;
    if (!--state->handles) ba_fabric_destroy(state);
}
/** --------------------------------------------------------------------------------------------------------- Close
 * @brief Stops event processing before releasing endpoints and registered memory.
 */
void ba_fabric_close(ba_net_peer* peer) {
    ba_fabric_state* state = peer->fabric;
    if (state->closing) return;
    state->closing = 1;
    if (state->endpoint) fi_shutdown(state->endpoint, 0);
    if (state->event_initialized) {
        uv_poll_stop(&state->event_poll);
        uv_close((uv_handle_t*)&state->event_poll, ba_fabric_poll_closed);
    }
    if (state->completion_initialized) {
        uv_poll_stop(&state->completion_poll);
        uv_close((uv_handle_t*)&state->completion_poll, ba_fabric_poll_closed);
    }
    if (state->prepare_initialized) {
        uv_prepare_stop(&state->prepare);
        uv_close((uv_handle_t*)&state->prepare, ba_fabric_poll_closed);
    }
    if (state->progress_initialized) {
        uv_idle_stop(&state->progress);
        uv_close((uv_handle_t*)&state->progress, ba_fabric_poll_closed);
    }
    if (!state->handles) ba_fabric_destroy(state);
}
/** --------------------------------------------------------------------------------------------------------- Receive
 * @brief Posts one registered receive buffer before accepting or connecting the endpoint.
 */
static int ba_fabric_receive(ba_fabric_state* state) {
    return (int)fi_recv(state->endpoint, state->receive, state->chunk,
                        fi_mr_desc(state->receive_memory), FI_ADDR_UNSPEC,
                        &state->receive_context);
}
/** --------------------------------------------------------------------------------------------------------- Transmit
 * @brief Posts the next frame fragment within the provider's maximum message size.
 */
static int ba_fabric_transmit(ba_fabric_state* state) {
    if (!state->transmit.data || state->transmitting) return 0;
    state->pending = state->transmit.size - state->sent;
    if (state->pending > state->chunk) state->pending = state->chunk;
    int status =
        (int)fi_send(state->endpoint, state->transmit.data + state->sent, state->pending,
                     fi_mr_desc(state->transmit_memory), FI_ADDR_UNSPEC, &state->transmit_context);
    if (status == -FI_EAGAIN) return 0;
    if (!status) state->transmitting = 1;
    return status;
}
/** --------------------------------------------------------------------------------------------------------- Write
 * @brief Registers an encoded frame until its final transmit completion.
 */
int ba_fabric_write(ba_net_peer* peer, ba_net_frame* frame) {
    ba_fabric_state* state = peer->fabric;
    if (state->transmit.data) return UV_EBUSY;
    int status = fi_mr_reg(state->domain, frame->data, frame->size, FI_SEND, 0, 2, 0,
                           &state->transmit_memory, NULL);
    if (status) return ba_fabric_error(status);
    state->transmit = *frame;
    state->sent = 0;
    *frame = (ba_net_frame){0};
    ++peer->writes;
    status = ba_fabric_transmit(state);
    if (status) {
        ba_net_peer_close(peer, ba_fabric_error(status));
        return UV_EIO;
    }
    return 0;
}
/** --------------------------------------------------------------------------------------------------------- Completions
 * @brief Advances received stream fragments and retires transmitted registered frames.
 */
static void ba_fabric_drain_completions(ba_fabric_state* state) {
    if (!state->completions || state->closing) return;
    struct fi_cq_msg_entry entry;
    ssize_t result;
    while (!state->closing && (result = fi_cq_read(state->completions, &entry, 1)) > 0) {
        if (entry.op_context == &state->receive_context) {
            ba_net_stream(state->peer, state->receive, entry.len);
            if (!state->closing && !state->peer->finish_writes) {
                int status = ba_fabric_receive(state);
                if (status) ba_net_peer_close(state->peer, ba_fabric_error(status));
            }
        } else if (entry.op_context == &state->transmit_context && state->transmitting) {
            state->transmitting = 0;
            state->sent += state->pending;
            if (state->sent == state->transmit.size) {
                fi_close(&state->transmit_memory->fid);
                state->transmit_memory = NULL;
                free(state->transmit.data);
                state->transmit = (ba_net_frame){0};
                --state->peer->writes;
                if (state->peer->finish_writes) ba_net_peer_close(state->peer, 0);
            }
        }
    }
    if (state->closing) return;
    if (result == -FI_EAVAIL) {
        struct fi_cq_err_entry error = {0};
        fi_cq_readerr(state->completions, &error, 0);
        ba_net_peer_close(state->peer, ba_fabric_error(-error.err));
    } else if (result < 0 && result != -FI_EAGAIN) {
        ba_net_peer_close(state->peer, ba_fabric_error((int)result));
    } else {
        int status = ba_fabric_transmit(state);
        if (status) ba_net_peer_close(state->peer, ba_fabric_error(status));
    }
}
/** --------------------------------------------------------------------------------------------------------- Poll Events
 * @brief Dispatches provider connection events reported by the wait descriptor.
 */
static void ba_fabric_event_ready(uv_poll_t* poll, int status, int events) {
    (void)events;
    ba_fabric_state* state = poll->data;
    if (status) ba_net_peer_close(state->peer, status);
    else ba_fabric_drain_events(state);
}
/** --------------------------------------------------------------------------------------------------------- Poll Completions
 * @brief Dispatches provider data completions reported by the wait descriptor.
 */
static void ba_fabric_completion_ready(uv_poll_t* poll, int status, int events) {
    (void)events;
    ba_fabric_state* state = poll->data;
    if (status) ba_net_peer_close(state->peer, status);
    else ba_fabric_drain_completions(state);
}
/** --------------------------------------------------------------------------------------------------------- Prepare Wait
 * @brief Progresses provider queues before allowing libuv to sleep on their descriptors.
 */
static void ba_fabric_progress(uv_idle_t* progress) { (void)progress; }
/** --------------------------------------------------------------------------------------------------------- Prepare
 * @brief Sleeps only after the provider has armed its wait descriptors successfully.
 */
static void ba_fabric_prepare(uv_prepare_t* prepare) {
    ba_fabric_state* state = prepare->data;
    struct fid* descriptors[2] = {&state->events->fid,
                                  state->completions ? &state->completions->fid : NULL};
    int status = fi_trywait(state->root->handle, descriptors, state->completions ? 2 : 1);
    if (status == -FI_EAGAIN) {
        ba_fabric_drain_events(state);
        ba_fabric_drain_completions(state);
        if (!state->closing) uv_idle_start(&state->progress, ba_fabric_progress);
    } else if (status) ba_net_peer_close(state->peer, ba_fabric_error(status));
    else uv_idle_stop(&state->progress);
}
/** --------------------------------------------------------------------------------------------------------- Start Polling
 * @brief Connects libfabric wait descriptors to the existing network reactor.
 */
static int ba_fabric_poll(ba_fabric_state* state) {
    int descriptor;
    int status = fi_control(&state->events->fid, FI_GETWAIT, &descriptor);
    if (status) return ba_fabric_error(status);
    status = uv_poll_init(state->peer->loop, &state->event_poll, descriptor);
    if (status) return status;
    state->event_poll.data = state;
    state->event_initialized = 1;
    ++state->handles;
    status = uv_poll_start(&state->event_poll, UV_READABLE, ba_fabric_event_ready);
    if (status) return status;
    if (state->completions) {
        status = fi_control(&state->completions->fid, FI_GETWAIT, &descriptor);
        if (status) return ba_fabric_error(status);
        status = uv_poll_init(state->peer->loop, &state->completion_poll, descriptor);
        if (status) return status;
        state->completion_poll.data = state;
        state->completion_initialized = 1;
        ++state->handles;
        status = uv_poll_start(&state->completion_poll, UV_READABLE, ba_fabric_completion_ready);
        if (status) return status;
    }
    status = uv_idle_init(state->peer->loop, &state->progress);
    if (status) return status;
    state->progress.data = state;
    state->progress_initialized = 1;
    ++state->handles;
    status = uv_prepare_init(state->peer->loop, &state->prepare);
    if (status) return status;
    state->prepare.data = state;
    state->prepare_initialized = 1;
    ++state->handles;
    return uv_prepare_start(&state->prepare, ba_fabric_prepare);
}
/** --------------------------------------------------------------------------------------------------------- Open State
 * @brief Creates a provider event queue and shares or initializes its fabric root.
 */
static int ba_fabric_open(ba_net_peer* peer, struct fi_info* info, ba_fabric_root* root) {
    ba_fabric_state* state = calloc(1, sizeof(*state));
    if (!state) return UV_ENOMEM;
    state->peer = peer;
    peer->fabric = state;
    ba_net_peer_retain(peer);
    if (root) {
        state->root = root;
        ++root->references;
    } else {
        state->root = calloc(1, sizeof(*state->root));
        if (!state->root) return UV_ENOMEM;
        state->root->references = 1;
        int status = fi_fabric(info->fabric_attr, &state->root->handle, NULL);
        if (status) {
            free(state->root);
            state->root = NULL;
            return ba_fabric_error(status);
        }
    }
    struct fi_eq_attr attributes = {.wait_obj = FI_WAIT_FD};
    int status = fi_eq_open(state->root->handle, &attributes, &state->events, NULL);
    return status ? ba_fabric_error(status) : 0;
}
/** --------------------------------------------------------------------------------------------------------- Active Endpoint
 * @brief Prepares registered receive storage and a message endpoint for an exchange.
 */
static int ba_fabric_active(ba_net_peer* peer, struct fi_info* info, ba_fabric_root* root) {
    int status = ba_fabric_open(peer, info, root);
    if (status) return status;
    ba_fabric_state* state = peer->fabric;
    state->chunk = sizeof(state->receive);
    if (info->ep_attr->max_msg_size < state->chunk) state->chunk = info->ep_attr->max_msg_size;
    if (!state->chunk) return UV_ENOTSUP;
    status = fi_domain(state->root->handle, info, &state->domain, NULL);
    struct fi_cq_attr attributes = {
        .size = 128, .format = FI_CQ_FORMAT_MSG, .wait_obj = FI_WAIT_FD};
    if (!status) status = fi_cq_open(state->domain, &attributes, &state->completions, NULL);
    if (!status) status = fi_endpoint(state->domain, info, &state->endpoint, NULL);
    if (!status) status = fi_ep_bind(state->endpoint, &state->events->fid, 0);
    if (!status) status = fi_ep_bind(state->endpoint, &state->completions->fid, FI_SEND | FI_RECV);
    if (!status) status = fi_enable(state->endpoint);
    if (!status)
        status = fi_mr_reg(state->domain, state->receive, sizeof(state->receive), FI_RECV, 0, 1, 0,
                           &state->receive_memory, NULL);
    if (!status) status = ba_fabric_receive(state);
    if (status) return ba_fabric_error(status);
    return ba_fabric_poll(state);
}
/** --------------------------------------------------------------------------------------------------------- Connection Events
 * @brief Accepts connections and starts outgoing frames when provider setup completes.
 */
static void ba_fabric_drain_events(ba_fabric_state* state) {
    if (state->closing) return;
    struct fi_eq_cm_entry entry;
    uint32_t event;
    ssize_t result;
    while (!state->closing &&
           (result = fi_eq_read(state->events, &event, &entry, sizeof(entry), 0)) > 0) {
        if (event == FI_CONNREQ && state->passive) {
            ba_net_peer* peer =
                ba_net_peer_create(state->peer->loop, state->peer->port, state->peer->protocol);
            int status = UV_ENOMEM;
            if (peer) {
                peer->server = 1;
                peer->callback = state->peer->callback;
                memcpy(peer->key, state->peer->key, 32);
                status = ba_fabric_active(peer, entry.info, state->root);
                if (!status)
                    status = fi_accept(((ba_fabric_state*)peer->fabric)->endpoint, NULL, 0);
            }
            if (status) {
                fi_reject(state->passive, entry.info->handle, NULL, 0);
                if (peer) ba_net_peer_close(peer, UV_EIO);
            }
            fi_freeinfo(entry.info);
        } else if (event == FI_CONNECTED) {
            if (state->peer->outgoing.data) {
                int status = ba_fabric_write(state->peer, &state->peer->outgoing);
                if (status) ba_net_peer_close(state->peer, status);
            }
        } else if (event == FI_SHUTDOWN) ba_net_peer_close(state->peer, UV_EOF);
    }
    if (state->closing) return;
    if (result == -FI_EAVAIL) {
        struct fi_eq_err_entry error = {0};
        fi_eq_readerr(state->events, &error, 0);
        ba_net_peer_close(state->peer, ba_fabric_error(-error.err));
    } else if (result < 0 && result != -FI_EAGAIN)
        ba_net_peer_close(state->peer, ba_fabric_error((int)result));
}
/** --------------------------------------------------------------------------------------------------------- Resolve Provider
 * @brief Requests message-capable providers with local registered-memory support.
 */
static int ba_fabric_info(const char* address, uint16_t port, uint64_t flags,
                          struct fi_info** info) {
    struct fi_info* hints = fi_allocinfo();
    if (!hints) return UV_ENOMEM;
    hints->caps = FI_MSG;
    hints->mode = FI_CONTEXT;
    hints->ep_attr->type = FI_EP_MSG;
    hints->tx_attr->msg_order = FI_ORDER_SAS;
    hints->domain_attr->mr_mode = FI_MR_LOCAL | FI_MR_VIRT_ADDR | FI_MR_ALLOCATED | FI_MR_PROV_KEY;
    char service[6];
    snprintf(service, sizeof(service), "%u", port);
    int status = fi_getinfo(FI_VERSION(1, 11), address, service, flags, hints, info);
    fi_freeinfo(hints);
    return status ? ba_fabric_error(status) : 0;
}
/** --------------------------------------------------------------------------------------------------------- Listen
 * @brief Binds a passive libfabric endpoint to the requested service port.
 */
int ba_fabric_listen(ba_net_peer* peer) {
    struct fi_info* info = NULL;
    int status = ba_fabric_info(NULL, peer->port, FI_SOURCE, &info);
    if (status) return status;
    status = ba_fabric_open(peer, info, NULL);
    ba_fabric_state* state = peer->fabric;
    if (!status) status = fi_passive_ep(state->root->handle, info, &state->passive, NULL);
    if (!status) status = fi_pep_bind(state->passive, &state->events->fid, 0);
    if (!status) status = fi_listen(state->passive);
    if (!status) status = ba_fabric_poll(state);
    fi_freeinfo(info);
    return status;
}
/** --------------------------------------------------------------------------------------------------------- Connect
 * @brief Starts an internal message connection without adding a public connection API.
 */
int ba_fabric_connect(ba_net_peer* peer, const char* address) {
    struct fi_info* info = NULL;
    int status = ba_fabric_info(address, peer->port, 0, &info);
    if (status) return status;
    status = ba_fabric_active(peer, info, NULL);
    if (!status) {
        status = fi_connect(((ba_fabric_state*)peer->fabric)->endpoint, info->dest_addr, NULL, 0);
        if (status) status = ba_fabric_error(status);
    }
    fi_freeinfo(info);
    return status;
}
