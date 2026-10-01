# Isolated SliceMap candidates

Run `ruby tests/experiments/slice_map_candidates/prepare_map_variants.rb OUTPUT_DIRECTORY`
from any directory. The generator reads the current working `src/containers/slicemap.cpp`,
requires each edited block to match exactly once, and emits copies under the supplied output
directory. It does not modify production sources or headers and does not read Git history.
`manifest.json` records the input and generated SHA-256 digests.

| Candidate | Change | Expected scope |
| --- | --- | --- |
| `baseline` | Unmodified current implementation | Reference for every comparison |
| `deferred_payload_hazard` | Construct the payload guard only after finding a node | Removes an unused hazard-slot clear on misses |
| `combined_publication_counter` | Combine successful insertion's in-flight decrement and position allocation | Removes one atomic read-modify-write per new key |
| `map_bookkeeping` | Apply both independent changes | Measures their combined effect |

These are hypotheses for measurement, not claimed improvements. Each directory contains
`slicemap.cpp` for compilation against the unchanged production headers. The separate
`direct_publication_node` idea is omitted because `SliceMap::complete_publish` is declared
in the public header and cannot receive a node without a matching declaration change.

## Hazard guard argument

The baseline protects the state before searching and protects a payload before retaining
its Slice. Both steps and their memory orders remain unchanged. A miss never dereferences
a payload, so constructing its guard and clearing its slot on that path provide no
additional protection. Hits construct the payload guard before their first payload load,
and destruction still clears the payload guard before the state guard.

## Combined counter argument

`positions_` packs in-flight attempts in its high word and allocated positions in its low
word. A successful insertion owns one in-flight increment before linking its node. The
baseline then subtracts `kInflightStep` with release ordering and adds one with relaxed
ordering, obtaining the old low word as its unique position. The candidate adds unsigned
`size_t(1) - kInflightStep` in one release RMW and obtains that same kind of unique old
low word. Each modification is indivisible, so concurrent successful insertions continue
to obtain distinct positions; their order remains unspecified.

The combined operation follows the successful link CAS and retains the release ordering
needed by the resizer's acquire observation of the in-flight count. It removes the
intermediate state in which a linked insertion has left the in-flight count but has not
yet allocated a position. Position and cell publication still happen afterward with
release stores. Completion scanning already tolerates a reserved position whose cell or
published flag is not ready. Failed attempts retain their original decrement and do not
allocate positions. Resize triggering and all hazard operations remain unchanged.

The arithmetic argument assumes the baseline's existing packed-word population bound:
the low-word population must not overflow into the high word. This experiment neither
changes nor extends that limit. Concurrent growth, duplicate-key insertion, publication
hooks, waiters, replacement, and retained-reader lifetime checks remain required before
interpreting timing results; the argument alone is not production qualification.
