# Slice copy candidate

Run `ruby tests/experiments/slice_map_candidates/prepare_slice_variants.rb OUTPUT_DIRECTORY`
to generate two complete translation units from the current `src/memory/slice.cpp`:

- `slice_baseline.cpp`: unchanged production source.
- `slice_fused_metadata_clone.cpp`: only the Slice copy constructor resolves each
  source and destination Region once, then accesses their entry and GPUBuf arrays.

The generator requires exactly one match for the current constructor and records
input and output SHA-256 hashes in `slice_variants.json`.

The candidate keeps `next_id`, null handling, placement resolution, token retention,
owner initialization, and GPUBuf assignment unchanged. Each copy still claims a
distinct ID and independent metadata; no public header or four-byte Slice layout
changes. Both Region loads retain acquire ordering. The live source owns its
metadata, and the newly claimed destination slot remains exclusive until publication.

The hypothesis is that eliminating duplicate Region resolution and out-of-line
entry/GPUBuf calls reduces retained-copy overhead. Slot allocation and reference
count operations remain unchanged, so this candidate does not address their cost
or contention. A compiler that already combines this work may show no improvement.

Compile each generated unit with identical production flags and link its object
before the original static library, excluding the original `slice.cpp` object from
extraction. Use the real public API for both correctness and performance runs;
the generated sources are experiments, not replacements for production files.

Before interpreting timing, require the existing Slice copy/view, cross-region,
map replacement/reset, and shader lifetime checks to pass with the candidate.
This matrix measures retained map hits with the same output lifetime schedule;
a dedicated retained-copy experiment is still needed to isolate clone cost.
