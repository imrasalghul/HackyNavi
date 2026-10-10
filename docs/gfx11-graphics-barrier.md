# GFX11 graphics cache barrier prerequisite

`gfx11_graphics_barrier` encodes and checks a fixed, 16-DWORD command sequence
for the Navi31 graphics PFP/ME queue. It is a real PM4 barrier intended for a
future privately captured draw IB. No current native call site submits it.

The first packet inserts `CACHE_FLUSH_AND_INV_TS_EVENT`, event index 5, with
PWS enabled. The event drains graphics/shader work and flushes and invalidates
CB/DB; forward sequencing then writes back and invalidates L2 and its metadata
cache. The adjacent PWS `ACQUIRE_MEM` waits for that last TS event at PFP,
distance zero. It covers the full GCR address range and carries no additional
cache operation. The acquire synchronizes the command frontend before later
commands proceed. There is no memory address, interrupt or CPU fence value in
either packet.

The pair must remain adjacent in one immutable trusted IB under the existing
sole-producer rule. PWS counters are hardware event counters; another process
inserting a TS event between the pair could change which event distance zero
refers to. This implementation accepts no caller-selected event, field, stage,
address or cache mask. It checks only this canonical slice, not arbitrary PM4.

This profile does **not** invalidate instruction, scalar, vector or L1 caches
for a subsequent shader reader. Such a consumer still needs its corresponding
cache invalidation. CPU readback still requires the native owner's actual
GPU-written final fence, consumed ring pointer, CPU visibility operation and
retained resource/VM ownership. An encoding or validation result is never a
hardware-completion result. The barrier does not prove CP DMA, another engine,
display scanout or every device consumer is idle.

## Source and existing fence distinction

The implementation follows Mesa commit
`35b085c4c067f198ce0b6437d94678e47b9c645f`:

- `src/amd/common/ac_barrier.c`: `ac_gfx10_emit_barrier`, with
  `SYNC_AND_INV_CB | SYNC_AND_INV_DB | INV_L2 | PFP_SYNC_ME` and the explicit
  PFP acquire point. The source explains that TS events idle shaders and that
  CB/DB must flush before L2. GLM writeback always includes invalidation.
- `src/amd/common/ac_cmdbuf_cp.c`: `ac_emit_cp_release_mem_pws` and
  `ac_emit_cp_acquire_mem_pws`, including the distinct RELEASE GCR encoding,
  `PWS_ENA2`, `PWS_ENA`, full PWS GCR size and TS counter selection.
- `src/amd/packets/cp_pm4_table_data_gfx11.json` and
  `src/amd/registers/pkt3.json`: actual packet field layouts.

The existing `src/gfx_queue.cpp` final fence already uses Linux
`gfx_v11_0_ring_emit_fence` from commit
`3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`. Its final RELEASE_MEM has the full
`CACHE_FLUSH_AND_INV_TS_EVENT` and forward metadata/L2 writeback; it is not a
compute-only fence. The isolated queue path also emits a CS partial flush,
but that extra packet does not restrict what the final TS event drains.

What is missing is graphics admission and its connected resource/retirement
contract. The queue's current capture path admits compute, and a final fence
does not by itself validate a draw, establish target ownership or authorize
freeing a graphics VM. The new PWS pair supplies an explicit in-IB ordering
point before subsequent commands and the queue-owned final fence. It neither
replaces that fence nor activates a new submission mode.

## Verification

`scripts/check-gfx11-graphics-barrier.py` runs the actual pinned Mesa
`ac_emit_barrier` implementation from the existing `libamd_common.a` in a small
host program. Its output is exactly:

```text
c0064900 80703514 00000000 00000000 00000000 00000000 00000000 00000000
c0065800 00022000 ffffffff 01ffffff 00000000 00000000 80000000 00000000
```

The independent oracle output, its executable and all twelve reused archive
bytes are hash-bound. The runner verifies the prior reviewed fragment build
identity and source manifest, and preflights/rechecks the actual oracle's 89
included dependencies. It reuses existing libraries without rebuilding or
claiming to reconstruct their original provenance. The dependency categories
distinguish pinned Mesa headers, generated build headers and toolchain files.

`build/gfx11-graphics-barrier-local/report.json` records nine successful local
commands: actual oracle compile/link/execution, strict and undefined-sanitizer
test builds/runs, and a freestanding x86_64 macOS 15 object compile with a 2 KiB
frame limit. Each test run passed 590 checks, including comparison with actual
Mesa output, every one-bit mutation of the sequence, all truncated lengths,
extra words, invalid pointers/alignment, output preservation and guard words.
The ordinary portable suite can also run without the external oracle file;
it retains the separately verified golden sequence.

The library and portable test are registered in CMake and the host test list;
the portable source is also registered in the kext compile list without a
submission call site. A fresh CMake build of only this target and its CTest
entry passed in `build/gfx11-graphics-barrier-cmake/report.json`. A native
combined driver build remains separate evidence.

```powershell
C:/Python314/python.exe scripts/check-gfx11-graphics-barrier.py --output build/gfx11-graphics-barrier-new
```

No remote build, GPU execution, Metal object, draw, image readback, render
encoder or system display functionality was exercised by this validation.
