/** --------------------------------------------------------------------------------------------------------- Slice Queue
 * @file ba_queue.c
 * @brief Transfers thread-local Slice blocks through sharded mailboxes and semaphore wakeups.
 */
#include "ba_queue.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#include <malloc.h>
#else
#include <pthread.h>
#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#else
#include <errno.h>
#include <semaphore.h>
#endif
#endif

#if defined(_WIN32)
typedef HANDLE ba_queue_semaphore_t;
typedef SRWLOCK ba_queue_mutex_t;
static void semaphore_init(ba_queue_semaphore_t* semaphore, size_t count) {
    *semaphore = CreateSemaphoreW(NULL, (LONG)count, LONG_MAX, NULL);
    if (!*semaphore) abort();
}
static void semaphore_wait(ba_queue_semaphore_t* semaphore) {
    if (WaitForSingleObject(*semaphore, INFINITE) != WAIT_OBJECT_0) abort();
}
static void semaphore_signal(ba_queue_semaphore_t* semaphore) {
    if (!ReleaseSemaphore(*semaphore, 1, NULL)) abort();
}
static void semaphore_destroy(ba_queue_semaphore_t* semaphore) { if (!CloseHandle(*semaphore)) abort(); }
static void mutex_init(ba_queue_mutex_t* mutex) { InitializeSRWLock(mutex); }
static void mutex_destroy(ba_queue_mutex_t* mutex) { (void)mutex; }
static void mutex_lock(ba_queue_mutex_t* mutex) { AcquireSRWLockExclusive(mutex); }
static void mutex_unlock(ba_queue_mutex_t* mutex) { ReleaseSRWLockExclusive(mutex); }
static void* queue_allocate(size_t bytes) { return _aligned_malloc(bytes, 128); }
static void queue_free(void* pointer) { _aligned_free(pointer); }
#else
typedef pthread_mutex_t ba_queue_mutex_t;
static void mutex_init(ba_queue_mutex_t* mutex) { if (pthread_mutex_init(mutex, NULL)) abort(); }
static void mutex_destroy(ba_queue_mutex_t* mutex) { if (pthread_mutex_destroy(mutex)) abort(); }
static void mutex_lock(ba_queue_mutex_t* mutex) { if (pthread_mutex_lock(mutex)) abort(); }
static void mutex_unlock(ba_queue_mutex_t* mutex) { if (pthread_mutex_unlock(mutex)) abort(); }
static void* queue_allocate(size_t bytes) { return aligned_alloc(128, bytes); }
static void queue_free(void* pointer) { free(pointer); }
#if defined(__APPLE__)
typedef dispatch_semaphore_t ba_queue_semaphore_t;
static void semaphore_init(ba_queue_semaphore_t* semaphore, size_t count) {
    *semaphore = dispatch_semaphore_create((long)count);
    if (!*semaphore) abort();
}
static void semaphore_wait(ba_queue_semaphore_t* semaphore) {
    if (dispatch_semaphore_wait(*semaphore, DISPATCH_TIME_FOREVER)) abort();
}
static void semaphore_signal(ba_queue_semaphore_t* semaphore) { dispatch_semaphore_signal(*semaphore); }
static void semaphore_destroy(ba_queue_semaphore_t* semaphore) { dispatch_release(*semaphore); }
#else
typedef sem_t ba_queue_semaphore_t;
static void semaphore_init(ba_queue_semaphore_t* semaphore, size_t count) {
    if (sem_init(semaphore, 0, (unsigned)count)) abort();
}
static void semaphore_wait(ba_queue_semaphore_t* semaphore) {
    while (sem_wait(semaphore)) if (errno != EINTR) abort();
}
static void semaphore_signal(ba_queue_semaphore_t* semaphore) { if (sem_post(semaphore)) abort(); }
static void semaphore_destroy(ba_queue_semaphore_t* semaphore) { if (sem_destroy(semaphore)) abort(); }
#endif
#endif
typedef struct ba_queue_lane ba_queue_lane_t;
/** --------------------------------------------------------------------------------------------------------- Block
 * @brief Carries privately owned descriptors between publication and recycling.
 */
typedef struct ba_queue_block {
    struct ba_queue_block* next;
    ba_queue_lane_t* owner;
    size_t count;
    _Alignas(128) ba_slice_t values[BA_QUEUE_BATCH_SIZE];
} ba_queue_block_t;
/** --------------------------------------------------------------------------------------------------------- Lane
 * @brief Owns the producer's bounded free block pool and available-space semaphore.
 */
struct ba_queue_lane {
    BA_ALIGN(128) ba_queue_mutex_t mutex;
    ba_queue_block_t* free;
    ba_queue_block_t* storage;
    ba_queue_semaphore_t available;
    int bound;
};
/** --------------------------------------------------------------------------------------------------------- Mailbox
 * @brief Publishes whole block lists to a preferred consumer with stealing at block boundaries.
 */
typedef struct ba_queue_mailbox {
    BA_ALIGN(128) ba_queue_mutex_t mutex;
    ba_queue_block_t* first;
    ba_queue_block_t* last;
    ba_queue_semaphore_t wake;
    int waiting;
    int bound;
} ba_queue_mailbox_t;
/** --------------------------------------------------------------------------------------------------------- Queue
 * @brief Spreads ready lists across consumers and publishes closure once per round.
 */
struct ba_queue {
    _Alignas(128) _Atomic int closed;
    ba_queue_lane_t* lanes;
    ba_queue_mailbox_t* mailboxes;
    size_t producers;
    size_t consumers;
    size_t blocks_per_lane;
};
/** --------------------------------------------------------------------------------------------------------- Local
 * @brief Keeps current producer and consumer blocks entirely private to their threads.
 */
struct ba_queue_local {
    ba_queue_t* queue;
    ba_queue_lane_t* lane;
    ba_queue_block_t* block;
    size_t offset;
    size_t receiver;
};
static _Thread_local ba_queue_local_t producer_local;
static _Thread_local ba_queue_local_t consumer_local;
/** --------------------------------------------------------------------------------------------------------- Create
 * @brief Initializes bounded producer pools and sleeping-consumer notifications.
 */
ba_queue_t* ba_queue_create(size_t producers, size_t consumers, size_t capacity) {
    if (!producers || !consumers || capacity < BA_QUEUE_BATCH_SIZE || capacity % BA_QUEUE_BATCH_SIZE) return NULL;
    const size_t blocks = capacity / BA_QUEUE_BATCH_SIZE;
    if (producers > SIZE_MAX / sizeof(ba_queue_lane_t) || consumers > SIZE_MAX / sizeof(ba_queue_mailbox_t)
        || blocks > SIZE_MAX / sizeof(ba_queue_block_t) || blocks > INT_MAX) return NULL;
    ba_queue_t* queue = queue_allocate(sizeof(*queue));
    if (!queue) abort();
    memset(queue, 0, sizeof(*queue));
    queue->producers = producers;
    queue->consumers = consumers;
    queue->blocks_per_lane = capacity / BA_QUEUE_BATCH_SIZE;
    queue->lanes = queue_allocate(producers * sizeof(ba_queue_lane_t));
    queue->mailboxes = queue_allocate(consumers * sizeof(ba_queue_mailbox_t));
    if (!queue->lanes || !queue->mailboxes) abort();
    atomic_init(&queue->closed, 0);
    for (size_t consumer = 0; consumer < consumers; ++consumer) {
        ba_queue_mailbox_t* mailbox = &queue->mailboxes[consumer];
        mutex_init(&mailbox->mutex);
        mailbox->first = mailbox->last = NULL;
        mailbox->waiting = 0;
        mailbox->bound = 0;
        semaphore_init(&mailbox->wake, 0);
    }
    for (size_t producer = 0; producer < producers; ++producer) {
        ba_queue_lane_t* lane = &queue->lanes[producer];
        mutex_init(&lane->mutex);
        lane->bound = 0;
        semaphore_init(&lane->available, queue->blocks_per_lane);
        lane->storage = queue_allocate(queue->blocks_per_lane * sizeof(ba_queue_block_t));
        if (!lane->storage) abort();
        lane->free = NULL;
        for (size_t index = 0; index < queue->blocks_per_lane; ++index) {
            ba_queue_block_t* block = &lane->storage[index];
            block->owner = lane;
            block->next = lane->free;
            lane->free = block;
        }
    }
    return queue;
}
/** --------------------------------------------------------------------------------------------------------- Destroy
 * @brief Releases queued ownership and checks complete recycling after all bindings are gone.
 */
void ba_queue_destroy(ba_queue_t* queue) {
    for (size_t producer = 0; producer < queue->producers; ++producer) {
        if (queue->lanes[producer].bound) abort();
    }
    for (size_t consumer = 0; consumer < queue->consumers; ++consumer) {
        ba_queue_mailbox_t* mailbox = &queue->mailboxes[consumer];
        if (mailbox->bound || mailbox->waiting) abort();
        ba_queue_block_t* block = mailbox->first;
        while (block) {
            ba_queue_block_t* next = block->next;
            for (size_t index = 0; index < block->count; ++index) ba_release(&block->values[index]);
            block->next = block->owner->free;
            block->owner->free = block;
            semaphore_signal(&block->owner->available);
            block = next;
        }
        mailbox->first = mailbox->last = NULL;
    }
    for (size_t producer = 0; producer < queue->producers; ++producer) {
        ba_queue_lane_t* lane = &queue->lanes[producer];
        size_t count = 0;
        for (ba_queue_block_t* block = lane->free; block; block = block->next) ++count;
        if (count != queue->blocks_per_lane) abort();
        mutex_destroy(&lane->mutex);
        semaphore_destroy(&lane->available);
        queue_free(lane->storage);
    }
    for (size_t consumer = 0; consumer < queue->consumers; ++consumer) {
        ba_queue_mailbox_t* mailbox = &queue->mailboxes[consumer];
        mutex_destroy(&mailbox->mutex);
        semaphore_destroy(&mailbox->wake);
    }
    queue_free(queue->mailboxes);
    queue_free(queue->lanes);
    queue_free(queue);
}
/** --------------------------------------------------------------------------------------------------------- Bind Producer
 * @brief Resolves actual TLS once so individual pushes avoid repeated TLS lookups.
 */
ba_queue_local_t* ba_queue_bind_producer(ba_queue_t* queue, size_t producer) {
    if (producer_local.queue || producer >= queue->producers
        || atomic_load_explicit(&queue->closed, memory_order_acquire)) return NULL;
    ba_queue_lane_t* lane = &queue->lanes[producer];
    mutex_lock(&lane->mutex);
    if (lane->bound) { mutex_unlock(&lane->mutex); return NULL; }
    lane->bound = 1;
    mutex_unlock(&lane->mutex);
    producer_local = (ba_queue_local_t){.queue = queue, .lane = &queue->lanes[producer],
        .receiver = producer % queue->consumers};
    return &producer_local;
}
/** --------------------------------------------------------------------------------------------------------- Bind Consumer
 * @brief Resolves actual TLS once so individual pops avoid repeated TLS lookups.
 */
ba_queue_local_t* ba_queue_bind_consumer(ba_queue_t* queue, size_t consumer) {
    if (consumer_local.queue || consumer >= queue->consumers) return NULL;
    ba_queue_mailbox_t* mailbox = &queue->mailboxes[consumer];
    mutex_lock(&mailbox->mutex);
    if (mailbox->bound) { mutex_unlock(&mailbox->mutex); return NULL; }
    mailbox->bound = 1;
    mutex_unlock(&mailbox->mutex);
    consumer_local = (ba_queue_local_t){.queue = queue, .receiver = consumer};
    return &consumer_local;
}
/** --------------------------------------------------------------------------------------------------------- Unbind
 * @brief Detaches a flushed producer or a drained consumer before its thread or queue exits.
 */
void ba_queue_unbind(ba_queue_local_t* local) {
    if (!local || !local->queue || (local != &producer_local && local != &consumer_local)) abort();
    if (local->lane) {
        ba_queue_flush(local);
        mutex_lock(&local->lane->mutex);
        local->lane->bound = 0;
        mutex_unlock(&local->lane->mutex);
    } else {
        if (local->block) abort();
        ba_queue_mailbox_t* mailbox = &local->queue->mailboxes[local->receiver];
        mutex_lock(&mailbox->mutex);
        mailbox->bound = 0;
        mutex_unlock(&mailbox->mutex);
    }
    *local = (ba_queue_local_t){0};
}
/** --------------------------------------------------------------------------------------------------------- Start
 * @brief Checks complete local draining before another queue round resumes.
 */
void ba_queue_start(ba_queue_local_t* local) {
    if (local->block) abort();
    local->offset = 0;
}
/** --------------------------------------------------------------------------------------------------------- Flush
 * @brief Publishes a private block to the next mailbox and wakes its consumer if sleeping.
 */
void ba_queue_flush(ba_queue_local_t* local) {
    ba_queue_block_t* block = local->block;
    if (!block) return;
    ba_queue_t* queue = local->queue;
    block->count = local->offset;
    block->next = NULL;
    ba_queue_mailbox_t* mailbox = &queue->mailboxes[local->receiver];
    mutex_lock(&mailbox->mutex);
    if (mailbox->last) mailbox->last->next = block;
    else mailbox->first = block;
    mailbox->last = block;
    if (mailbox->waiting) {
        mailbox->waiting = 0;
        semaphore_signal(&mailbox->wake);
    }
    mutex_unlock(&mailbox->mutex);
    local->receiver = (local->receiver + 1) % queue->consumers;
    local->block = NULL;
    local->offset = 0;
}
/** --------------------------------------------------------------------------------------------------------- Push
 * @brief Copies locally until a full block requires one synchronized publication.
 */
size_t ba_queue_push(ba_queue_local_t* local, const ba_slice_t* input, size_t count) {
    size_t written = 0;
    while (written < count) {
        if (!local->block) {
            ba_queue_lane_t* lane = local->lane;
            semaphore_wait(&lane->available);
            mutex_lock(&lane->mutex);
            local->block = lane->free;
            lane->free = lane->free->next;
            mutex_unlock(&lane->mutex);
        }
        size_t amount = BA_QUEUE_BATCH_SIZE - local->offset;
        if (amount > count - written) amount = count - written;
        if (amount == 1) local->block->values[local->offset] = input[written];
        else memcpy(local->block->values + local->offset, input + written, amount * sizeof(ba_slice_t));
        local->offset += amount;
        written += amount;
        if (local->offset == BA_QUEUE_BATCH_SIZE) ba_queue_flush(local);
    }
    return written;
}
/** --------------------------------------------------------------------------------------------------------- Acquire Blocks
 * @brief Detaches ready lists or registers a wake under the same mutex that publishes work.
 */
static ba_queue_block_t* acquire_blocks(ba_queue_local_t* local) {
    ba_queue_t* queue = local->queue;
    for (;;) {
        const int closed = atomic_load_explicit(&queue->closed, memory_order_acquire);
        for (size_t scanned = 0; scanned < queue->consumers; ++scanned) {
            ba_queue_mailbox_t* mailbox = &queue->mailboxes[(local->receiver + scanned) % queue->consumers];
            mutex_lock(&mailbox->mutex);
            ba_queue_block_t* first = mailbox->first;
            mailbox->first = mailbox->last = NULL;
            mutex_unlock(&mailbox->mutex);
            if (first) return first;
        }
        if (closed) return NULL;
        ba_queue_mailbox_t* home = &queue->mailboxes[local->receiver];
        mutex_lock(&home->mutex);
        if (home->first || atomic_load_explicit(&queue->closed, memory_order_acquire)) {
            mutex_unlock(&home->mutex);
            continue;
        }
        home->waiting = 1;
        mutex_unlock(&home->mutex);
        semaphore_wait(&home->wake);
    }
}
/** --------------------------------------------------------------------------------------------------------- Pop
 * @brief Drains exclusively owned block lists before recycling each block to its producer.
 */
size_t ba_queue_pop(ba_queue_local_t* local, ba_slice_t* output, size_t count) {
    if (!count) return 0;
    if (!local->block) {
        local->block = acquire_blocks(local);
        local->offset = 0;
        if (!local->block) return 0;
    }
    ba_queue_block_t* block = local->block;
    size_t amount = block->count - local->offset;
    if (amount > count) amount = count;
    if (amount == 1) output[0] = block->values[local->offset];
    else memcpy(output, block->values + local->offset, amount * sizeof(ba_slice_t));
    local->offset += amount;
    if (local->offset == block->count) {
        ba_queue_lane_t* lane = block->owner;
        ba_queue_block_t* next = block->next;
        mutex_lock(&lane->mutex);
        block->next = lane->free;
        lane->free = block;
        mutex_unlock(&lane->mutex);
        local->block = next;
        local->offset = 0;
        semaphore_signal(&lane->available);
    }
    return amount;
}
/** --------------------------------------------------------------------------------------------------------- Close
 * @brief Publishes closure and wakes registered sleepers after every producer flushes.
 */
void ba_queue_close(ba_queue_t* queue) {
    atomic_store_explicit(&queue->closed, 1, memory_order_release);
    for (size_t consumer = 0; consumer < queue->consumers; ++consumer) {
        ba_queue_mailbox_t* mailbox = &queue->mailboxes[consumer];
        mutex_lock(&mailbox->mutex);
        if (mailbox->waiting) {
            mailbox->waiting = 0;
            semaphore_signal(&mailbox->wake);
        }
        mutex_unlock(&mailbox->mutex);
    }
}
/** --------------------------------------------------------------------------------------------------------- Reset
 * @brief Reopens a fully drained queue between synchronized benchmark rounds.
 */
void ba_queue_reset(ba_queue_t* queue) {
    for (size_t consumer = 0; consumer < queue->consumers; ++consumer) {
        if (queue->mailboxes[consumer].first || queue->mailboxes[consumer].waiting) abort();
    }
    for (size_t producer = 0; producer < queue->producers; ++producer) {
        size_t count = 0;
        for (ba_queue_block_t* block = queue->lanes[producer].free; block; block = block->next) ++count;
        if (count != queue->blocks_per_lane) abort();
    }
    atomic_store_explicit(&queue->closed, 0, memory_order_relaxed);
}
