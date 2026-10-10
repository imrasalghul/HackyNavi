# Atomic native graphics capture

The native owner integration for the position-only NGG plus constant-color PS
profile is implemented in `HackyNaviVMGraphics.cpp`. The shader validator, color
calculator, harvested topology and fixed draw encoder are separate inputs to
that ownership boundary. Initial real kernel-header compilation of six affected
VM/queue translation units passes with the 2 KiB stack-frame limit; final native
integration review and target validation are still pending. The dependency uses
the corrected GFX11 CB encoder in `gfx11-graphics-draw-local-4`. No draw
has been submitted by this validation, and it does not prove target hardware
execution or general Metal rendering.

## Owner and admission

`HackyNaviVMContext::captureProductGraphics(Operation&, request)` is an
internal kernel interface. Its caller installs the exact GART normal Access
before entering the shared mutex. The method validates mutable context/VMID
ownership, the current GART creation/Operation generation and the private
completed topology capture; a supplied numeric topology/report is never an
authority argument. VM's resource epoch is separate from the FunctionGroup
generation and must not be compared as though they were the same counter.

The request supplies a kernel-readable canonical image plus its two stage
lengths, target BO handle/generation, a page-aligned target offset, dimensions
and address-free layout. No caller GPU address, register image, resource permission,
shader report, completion assertion or attribute-ring handle is accepted.
User-client code must copy/resolve inputs into kernel ownership first;
this pointer-bearing internal interface is not itself a wire ABI.

An allocated private `GraphicsCapture` contains the exact target binding,
layout, shader image admission, topology/Operation generation, same VM epoch,
draw request, command extent and all new internal BO handles. All code is
copied once into its actual private DMA page before validation. The caller's
code pointer is erased and never revisited. From the first retained allocation,
failure becomes `FailedUnpublished`, with partial objects remaining in the
owner ledger until explicit unpublished release or typed reset retirement.

After all owned resources and the actual command bytes validate, set the new
graphics shader class and `State::Captured`. Existing context sealing already
blocks CPU writes, free, map and unmap across every BO. Therefore the retained
color binding is private state of the atomic capture; a separate transferable
color-token object or a new weaker per-buffer lease is unnecessary. Merely
retaining a mutable VMContext would not suffice.

## Exact owned mappings

The initial profile has four admitted graphics bindings and no mapped aliases:

| Resource | Mapping | Ownership and size |
| --- | --- | --- |
| NGG/PS code image | Read + Execute | Private/internal, canonical 4096-byte image; NGG at zero and PS at next 256-byte boundary |
| Command IB | Read + Execute | Private/internal; at most 2048 DWORDs in an 8192-byte allocation; unused words remain zero and are revalidated |
| Color target | Read + Write | Exact external-data handle/generation; one mapping begins at target offset and covers exactly the page-rounded validated linear RGBA8 footprint |
| GE attribute ring | Read + Write | Separate private/internal BO, GPU VA aligned to 64 KiB, source-derived per-maximum-SE size |

The code page remains wholly mapped for gfx11's distance-three prefetch. Code
and attribute ring must lie completely within one selected 4 GiB VA window,
with a common `address32_hi`. Existing `controlVaLocked` does not provide this
contract; graphics requires a bounded geometry planner over the actual mapping
ledger. Placement has to consider all occupied ranges, arithmetic limits and
the entire ring extent, not just starting-address alignment. The current
16 MiB BO limit is retained.

Pinned Mesa's Navi31/L3 policy supplies 1400 KiB per maximum SE, rounded to
64 KiB: 1,441,792 bytes per SE. Six maximum SEs would need 8,650,752 bytes;
that example is not an observed value or an admission default. This profile
has no separate position or primitive ring. Actual maximum SE and harvesting
must be privately captured before allocating or encoding any of these values.

The capture proves that every retained BO and page-table storage object
has valid real DMA segments and distinct CPU and device I/O address intervals,
as the existing compute product capture does. Device-visible I/O addresses are
not described as CPU physical addresses. CPU allocations, I/O-bus segments and the metadata/work
scratch intervals require distinct checks; a BO-id inequality is insufficient.
There are no imported-memory aliases in this native allocator. All mapping
permissions must match exactly and all internal owners must appear in the
captured control mask.

Persistent unrelated data BOs may remain mapped, each through exactly one
non-internal Read or Read+Write mapping. The complete mapping ledger must retain
valid generation/extent/alignment/access, distinct VA ranges, and exact owner
mapping counts. Additional Execute mappings, aliases and duplicate BO mappings
are rejected. This complete data-ledger check runs before the first private
capture allocation and repeats on every capture validation. Global CPU/DMA
interval checks still include these allocations. This graphics-specific allowance
does not relax the existing compute-product admission, which rejects unbound
extra mappings.

## Submission, completion and retirement

The existing queue takes Queue → Firmware → GART locks, excludes other
producers, validates private capture contents, reserves immutable submission
identity, activates the actual VM and publishes real DMA before the IB.
Extend its capture admission to the new class only after the full fixed draw
encoder and ring-state contract are implemented and reviewed. Revalidation
must derive shader/layout/state from private bytes and private topology again;
it must not trust the previously returned numeric report.

GE attribute-ring updates need the actual adjacent BOTTOM_OF_PIPE_TS PWS
release plus CP_ME acquire sequence in the canonical stream before updating
ring state. Code/target caches, all fixed draw state and fence retirement belong
to that complete stream/queue protocol. Existing final queue TS fences already
cover the full pipeline; they are not compute-only fences.

The actual DMA-synchronized sequence and consumed RPTR issue the existing
completion object. Its issuer, context, capture generation, queue producer,
VMID, resource epoch and exact target result range remain bound. Neither
compilation nor CPU color propagation can generate a successful result.
Readback synchronizes the actual target allocation only after these checks.

`consumeCapturedStorageLocked` now uses a narrow exact permission classification
for this private attribute ring: code and IB remain RX, only the captured ring
is RW. Its precise handle/generation, mapping, extent and permissions must match;
the compute classes still reject every writable internal control. After genuine completed/drained
submission and both-hub detach, consumption removes only the exact private
code/IB/ring mappings and allocations; the target and VM root remain retained
for subsequent work. Ring state must not be reused by any later producer
without that producer installing its own admitted live ring. All partial
publication, fence, detach or cleanup failures preserve the existing hardware
holds and require the actual reset/retirement path; there is no retry.

## Retained topology contract

The GART owner captures GB_ADDR_CONFIG and NBIO revision, validates the actual
GC1.2 discovery table for maximum SE/SA/RB counts and performs a real serialized
harvesting capture. Both are one-shot operations within the issuing FunctionGroup
generation. Graphics capture calls the completed-record copy methods first;
only an absent record attempts initial capture. A failed attempted capture seals
the GART owner; it cannot overwrite the record or replay selector writes.
These methods run before taking the VM/GART lock, then the private record and
its issuing generation are checked again while that lock is held. Numeric copy
outputs are diagnostics, not inputs to draw binding authority.
Linux `gfx_v11_0_get_cu_info` selects each live SE/SA through GRBM_GFX_INDEX,
reads CC/USER WGP-inactive masks and expands each active WGP to two CU bits.
Global CC/USER SA and RB masks further constrain topology. Any selector
attempt/restoration must have private attempted/completed state and retained
ambiguity, under the same actual operation and device lock.

Mesa derives `num_tile_pipes = 1 << NUM_PIPES(GB_ADDR_CONFIG)` and gfx11
`min_good_cu_per_sa` by rounding the aggregate CU count down to the WGP
distribution quantum. It is not a minimum of individual masks. Linux sets
its `pa_sc_tile_steering_override` policy to zero in software; that policy
choice is distinct from observing a register. None of these values
may come from a caller-provided profile or a libdrm example.

The native capture revalidates its private code bytes and exact canonical IB,
all four bindings and unrelated data mappings, the issuing topology generation and actual DMA owner ledger
before submission and completion use. The GPU may change attribute-ring and
target contents; those contents are never presumed to remain zero. Readback is
restricted to the exact unrounded `layout.total_bytes`, starting at the original
target BO offset. All staged code, unused BOs and page tables remain included in
the storage-disjointness check even when they have no GPU mapping.
