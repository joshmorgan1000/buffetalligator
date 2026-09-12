#pragma once
/** --------------------------------------------------------------------------------------------------------- Slice Queue
 * @file ba_queue.h
 * @brief Declares a buffered C queue with thread-local blocks and semaphore handoffs.
 */
#include "ba_descriptor.h"

#define BA_QUEUE_BATCH_SIZE 256
typedef void (*ba_queue_release_fn)(void* descriptor);
typedef struct ba_queue ba_queue_t;
typedef struct ba_queue_local ba_queue_local_t;
/** --------------------------------------------------------------------------------------------------------- Create
 * @brief Allocates bounded producer block pools before workers start.
 */
ba_queue_t* ba_queue_create(size_t producers, size_t consumers, size_t capacity, ba_queue_release_fn release);
/** --------------------------------------------------------------------------------------------------------- Destroy
 * @brief Releases undelivered descriptors through the registered release callback and frees
 * storage after every thread binding is released.
 */
void ba_queue_destroy(ba_queue_t* queue);
/** --------------------------------------------------------------------------------------------------------- Bind Producer
 * @brief Binds this thread's producer-local state to one queue and lane.
 */
ba_queue_local_t* ba_queue_bind_producer(ba_queue_t* queue, size_t producer);
/** --------------------------------------------------------------------------------------------------------- Bind Consumer
 * @brief Binds this thread's consumer-local state to one queue.
 */
ba_queue_local_t* ba_queue_bind_consumer(ba_queue_t* queue, size_t consumer);
/** --------------------------------------------------------------------------------------------------------- Unbind
 * @brief Flushes a producer or detaches a drained consumer on its owning thread.
 */
void ba_queue_unbind(ba_queue_local_t* local);
/** --------------------------------------------------------------------------------------------------------- Start
 * @brief Resets a bound thread's local completion state before another drained round.
 */
void ba_queue_start(ba_queue_local_t* local);
/** --------------------------------------------------------------------------------------------------------- Push
 * @brief Moves descriptors into private blocks and publishes each full block.
 */
size_t ba_queue_push(ba_queue_local_t* local, const ba_slice_t* input, size_t count);
/** --------------------------------------------------------------------------------------------------------- Flush
 * @brief Publishes a producer's partial block at a caller-defined burst boundary.
 */
void ba_queue_flush(ba_queue_local_t* local);
/** --------------------------------------------------------------------------------------------------------- Pop
 * @brief Drains private block lists or sleeps until work or closure arrives.
 */
size_t ba_queue_pop(ba_queue_local_t* local, ba_slice_t* output, size_t count);
/** --------------------------------------------------------------------------------------------------------- Close
 * @brief Wakes all consumers after every producer has flushed its final block.
 */
void ba_queue_close(ba_queue_t* queue);
/** --------------------------------------------------------------------------------------------------------- Reset
 * @brief Reopens a fully drained queue while every participant is quiescent.
 */
void ba_queue_reset(ba_queue_t* queue);
