# Kitchen dispatch experiments

This directory compares the current Kitchen with three small persistent-worker executors; it changes no production source or public contract.

Run the build, checks, and serialized comparison with one command:

```sh
bash tests/experiments/kitchen_dispatch/run_build.sh
```

The wrapper uses the repository's `run_build.sh`, places the build in `build/kitchen-dispatch`, runs the registered checks, and saves measurements in `build/kitchen-dispatch/results`. It accepts optional build and results directories as its first two arguments. Run `ruby tests/experiments/kitchen_dispatch/analyze.rb RESULTS_DIRECTORY` to produce `summary.json` and `summary.txt`.

Each candidate has a shared Moodycamel compute queue and a shared storage queue, a hardware-sized prestarted team per queue, and scoped explicit producer tokens. The production adapter wraps borrowed experiment records in Orders and submits all work to Kitchen's single hardware-sized team.

| Executor | Queue and waiting behavior |
|---|---|
| `kitchen` | Actual production `submit` and `submit_bulk` on one queue |
| `blocking` | BlockingConcurrentQueue with its default 10,000 semaphore spins |
| `sleeping` | Same queue with `MAX_SEMA_SPINS=0` |
| `spinning` | ConcurrentQueue with continuous dequeue polling on both teams |

The harness completes accepted tasks and continuations before destroying an executor, then candidate shutdown sends one sentinel per worker and joins every thread. These are design experiments, not drop-in Kitchen replacements. Including the public Kitchen header also initializes its production pool, which remains unused in candidate processes.

`main.cpp` measures one-in-flight callbacks, idle wakeups, and bounded buffered reads. Each read is 64 KiB from a repeatedly reused 64 MiB file; setup/fsync precede timing and every payload byte is verified. Reads are cache-friendly and are not physical-device throughput measurements. Slice retention and promise creation are outside individual latency timestamps but included in throughput. Completion state is moved into the callback before publication, retaining it until notification returns.

`burst.cpp` measures completed single submissions and batches of 256, from one and twice the reported CPU count of producers. Every task publishes its identity in a separate hardware-probed cache line; no global completion RMW is added by the harness. Both timer and CPU interval exclude record reset and final payload verification. Borrowed records and completion storage are preallocated; the production adapter includes Order construction and its allocations in the measured interval. The observer waits for all completion flags and for all producers to finish enqueueing.

`io_isolation.cpp` occupies one worker per hardware thread with an empty pipe read, then submits an ordinary compute probe. An independent thread releases the reads at a deadline set 20 ms ahead, outside the probe's latency boundary. `shared` submits reads to compute; `separate` uses storage; `coroutine` suspends a small C++ coroutine for the storage read and queues its continuation onto compute. A compute Task completion hook publishes completion after the final resume has returned, so the controller can destroy its suspended frame safely. This proves compute isolation during pending I/O; it does not measure coroutine-request throughput or make `read()` nonblocking.

The runner executes each case in `kitchen/blocking/sleeping/spinning` order, followed by reverse order, using a fresh process each time and waiting for its full exit. Ordinary and burst cases retain one warmup plus three measured repetitions per process. Isolation cases retain 101 probe rounds per process. The 88 final processes produce 192 ordinary/burst sample rows and 24 isolation summaries. Reported ordinary p99 values are medians of six per-repetition p99 values; isolation p99 values are medians of two process p99 values. Whole-process time includes setup, warmup and shutdown, with roughly 50 ms controller resolution.

The controller emits progress every second and terminates its own process group after 120 seconds. Runtime hardware probing requires permission to read macOS `hw.cachelinesize` or Linux `_SC_LEVEL1_DCACHE_LINESIZE`; a denied probe fails explicitly. The current measurement used 128-byte cache lines, 16 compute workers, 16 storage workers, and 32 producers for concurrent cases.

October 3 results are in [the retained report](../../../build/performance/2026-10-03/queue-approaches/report.md). Those measurements used the previous Kitchen API and a host occupied by a separate CPU-heavy test; rerun the comparison before drawing conclusions about the current implementation.
