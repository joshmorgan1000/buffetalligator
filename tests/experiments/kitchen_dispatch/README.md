# Kitchen dispatch experiments

This directory compares the real Kitchen at commit `2fbaa03` with three small persistent-worker executors; it changes no production source or public contract.

Run the build, checks, and serialized comparison with one command:

```sh
bash tests/experiments/kitchen_dispatch/run_build.sh
```

The wrapper uses the repository's `run_build.sh`, places the build in `build/kitchen-dispatch`, runs the registered checks, and saves measurements in `build/kitchen-dispatch/results`. It accepts optional build and results directories as its first two arguments. Run `ruby tests/experiments/kitchen_dispatch/analyze.rb RESULTS_DIRECTORY` to produce `summary.json` and `summary.txt`.

Each candidate has a shared Moodycamel compute queue and a shared storage queue, a hardware-sized prestarted team per queue, scoped explicit producer tokens, and no executor-wide per-task accounting. The production adapter invokes the actual Kitchen; its worker growth, shared accounting, and waiter pool remain part of that baseline.

| Executor | Queue and waiting behavior |
|---|---|
| `kitchen` | Actual production `submit`, `submit_bulk`, and `submit_waiting` |
| `blocking` | BlockingConcurrentQueue with its default 10,000 semaphore spins |
| `sleeping` | Same queue with `MAX_SEMA_SPINS=0` |
| `spinning` | ConcurrentQueue with continuous dequeue polling on both teams |

Candidates intentionally omit production worker resizing and `drain()` accounting. The harness completes accepted tasks and continuations before destroying an executor, then shutdown sends one sentinel per worker and joins every thread. These are design experiments, not drop-in Kitchen replacements. Including the public Kitchen header also initializes its production pool, which remains unused in candidate processes.

`main.cpp` measures one-in-flight callbacks, idle wakeups, and bounded buffered reads. Each read is 64 KiB from a repeatedly reused 64 MiB file; setup/fsync precede timing and every payload byte is verified. Reads are cache-friendly and are not physical-device throughput measurements. Slice retention and promise creation are outside individual latency timestamps but included in throughput. Completion state is moved into the callback before publication, retaining it until notification returns.

`burst.cpp` measures completed single submissions and batches of 256, from one and twice the reported CPU count of producers. Every task publishes its identity in a separate hardware-probed cache line; no global completion RMW is added by the harness. Both timer and CPU interval exclude record reset and final payload verification. The task records and completion storage are preallocated, so this measures POD task dispatch rather than allocation-heavy application requests. The observer waits for all completion flags and for all producers to finish enqueueing.

`io_isolation.cpp` occupies one worker per hardware thread with an empty pipe read, then submits an ordinary compute probe. An independent thread releases the reads at a deadline set 20 ms ahead, outside the probe's latency boundary. `shared` submits reads to compute; `separate` uses storage; `coroutine` suspends a small C++ coroutine for the storage read and queues its continuation onto compute. A compute Task completion hook publishes completion after the final resume has returned, so the controller can destroy its suspended frame safely. This proves compute isolation during pending I/O; it does not measure coroutine-request throughput or make `read()` nonblocking.

The runner executes each case in `kitchen/blocking/sleeping/spinning` order, followed by reverse order, using a fresh process each time and waiting for its full exit. Ordinary and burst cases retain one warmup plus three measured repetitions per process. Isolation cases retain 101 probe rounds per process. The 88 final processes produce 192 ordinary/burst sample rows and 24 isolation summaries. Reported ordinary p99 values are medians of six per-repetition p99 values; isolation p99 values are medians of two process p99 values. Whole-process time includes setup, warmup and shutdown, with roughly 50 ms controller resolution.

The controller emits progress every second and terminates its own process group after 120 seconds. Runtime hardware probing requires permission to read macOS `hw.cachelinesize` or Linux `_SC_LEVEL1_DCACHE_LINESIZE`; a denied probe fails explicitly. The current measurement used 128-byte cache lines, 16 compute workers, 16 storage workers, and 32 producers for concurrent cases.

October 3 results are in [the retained report](../../../build/performance/2026-10-03/queue-approaches/report.md). A separate CPU-heavy test occupied the host throughout those measurements; use the controlled isolation result confidently, and repeat the throughput comparison on a quiet host before choosing a queue policy. Production remains at the baseline and the prior redesign is preserved in stash `a2e1f687c7c93814a291aba191441ea1bcfea0cf`.
