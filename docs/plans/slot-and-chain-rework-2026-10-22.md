# Slot and chain rework (2026-10-22)

Josh's design for the Slice slot, the chain buffets and the typed registry, as stated on 2026-10-21 and
2026-10-22, with the decisions made and the one still open. The working tree is fixed in place; the git
history after the last September 30 commit (`2fbaa03`) is not the reference, this document and Josh are.

## Main Goals

As always, performance, efficiency, and usability are the key pillars of this design.
**Do not write code that simply works.** This is intended for use in high-performance computing, and has
years of testing behind its design approaches. We are here because agents repeatedly clobber perfectly
good code, and sneak those modifications in with unrelated feature enhancements.

## Requirements, in order

1. **The current public ABI contracts must not be altered.** Many other projects depend on this one, so
   a small surface change can mean weeks of effort to adapt dependent projects.
2. A Slice is not a memory allocation, it is a claim on a pre-allocated buffer. A Slice *can* be an
   allocation if it is a novel Slice.
3. A Slice is 4 bytes, trivially copyable, and a shader translates it with the same table the host uses.
4. Everything else is reference counting and garbage collection: an index to a row that tells host and GPU
   where the bytes are, and an answer to "what happens when I am released".

## The ChainBuffet

**The ChainBuffet is sacred.**

The public header must not be touched. It has particular mechanisms that are non-negotiable:
- It always keeps at least one buffer ahead allocated at all times, with the exception of those which
  are novel buffers.
- The `next(bool)` method either returns the value of the `next_` atomic pointer, or it uses CAS to
  atomically swap it out with a new one. **This code has already been written.**
- Claims that are equal to or larger than the default buffer size are automatically given novel buffers.
- ChainBuffet objects begin with a reference count of one. When a claim attempt advances the bump
  pointer past the end of the current buffer, **it is considered full**. The atomic shall not be
  touched, `next()->claim(size)` is called instead. When that happens, the buffer for that descriptor
  type that is returned by the static `current_for` method is updated to point to the new buffer,
  and the current buffer then decrements its reference count. If the reference count reaches zero,
  then it uses the `Kitchen` to schedule itself for deletion after deallocating the buffer.

**Do not add any new methods to the ChainBuffet.** The contract is complete as-is.

## Decisions made

- **The ChainBuffet is sacred.** We are only changing out the token for an atomic ID that references a
  slot in the atomics table. **Do not add or remove 
- **The Alligator claims.** `Alligator::claim(size, novel, placement)` is the only path to claim a Slice.
  The Slice constructors, `resize`, `slice()` and `raw()` call into the Alligator; nothing outside the
  Alligator ever sees a `ChainBuffet`. There is no `ChainBuffet::chain` and no public `ChainBuffet::claim`,
  although there is a private `ChainBuffet::claim` used internally by the Alligator that returns a
  `SliceEntry`.
- **Placement is automatic.** Nothing specifies a placement up front; the default is the Vulkan placement
  on unified memory, host coherent otherwise, from the probe at startup. The Alligator strives to be
  automatic and intelligent about placement, and should automatically copy memory to/from  the host or
  device as needed without any extra commands from the ABI user.
- **The id is a pure index.** Three placement bits, six region bits, the slot within the region. The GLSL
  and Metal preludes decode it the same way: `directory[(id >> 3) & 63]`, `refs[id >> 9]`. No type or
  descriptor metadata lives in the id.
- **Copies share a parent slot.** Slices can be sub-sliced, which only increment their parent's reference
  count, not the parent of the parent. Otherwise many Slices end up pounding on the same atomic.
- **No token.** The actual atomics live on a slab and are accessed via an index an a `std::atomic_ref`.
- **Typed slots run their destructor.** `SliceTypes` is a table of `{construct, destroy, size}` records
  indexed by the type byte in the slot. `SliceT<T>` is one element of T and registers its record through a
  private static member on first instantiation, with no method to call. A typed claim constructs T; the last
  release destroys it, whichever handle that is, so a struct holding Slices (the "slice of slices" leak)
  frees them. `SliceT<Slice>` adopting a raw slot stamps it. `SliceV<T>` is the vector form, with a size and
  push or emplace semantics, and is a later step.
- **All Slice objects are guaranteed to be pre-zeroed and 64-byte aligned.**
- **`from_id` is the adopt path.** No second private adopting constructor.
- **Comment lines inside classes stay under 100 characters.** No one-line static helper methods.

## The slot

Per slot, the 16-byte GPUBuf (device address, granule length, granule offset) is fixed by the shaders. The
host entry carries the count and the type: `state` = `[31:24] type | [23:0] owners`, one 32-bit atomic.

A slot must hold the following metadata:
- A GPUBuf (device address, granule length, granule offset)
- A host `void*` pointer. Size metadata can be pulled from the GPUBuf.
- A reference to the underlying ChainBuffet or parent Slice.
- A reference to the atomic counter for said parent.

**Each element of the slot is stored in its own individual table.**

## Teardown

Lifetime phases stay: the Kitchen is joined first, then TrackerDetach, Registry, Allocator, Runtime. A link
released once the Kitchen has stopped retires in place; the Alligator sets its finalizing flag when its own
teardown begins and the current links are released there.

## Files

`include/alligator.hpp` (ChainBuffet, SliceTypes, SliceEntry, Region, Slice, SliceT, Alligator),
`src/memory/chainbuffet.cpp`, `src/memory/slice.cpp`, `src/memory/alligator.cpp`,
`include/alligator/easyvulkan.hpp` and `src/metal/prelude.hpp` (id decode), `src/memory/mmap_allocator.cpp`
and the tests that read `SliceEntry::from_slice(...)->token()->buffet()` (becomes `->buffet()`) and
`entry.owners.load(...)` (becomes `entry.owners()`).

## Nebula

Nebula's build clones the alligator from GitHub `main` and resets to it, so this reaches Nebula only once
pushed. Every alligator symbol Nebula uses predates Oct 13 except `from_id` and `discharge_unsafely`, which
stay. The SearchDetails leak in Nebula (nothing runs `~SearchDetails`) is what the typed slot fixes:
`SearchDetails::make_slice` becomes a typed claim.
