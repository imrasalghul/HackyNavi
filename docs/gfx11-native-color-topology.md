# Native color topology capture

`HackyNaviGARTController::captureColorTopology` reads the actual GC address
configuration and NBIO revision strap. It is an explicit prerequisite with no
startup callsite. It does not allocate a render target, program CB state, draw,
or establish Metal rendering support. No target GPU was accessed to validate
this implementation.

GART owns the record because it owns the actual provider/open owner, BAR5 map,
copied validated discovery and shared device mutex. The capture requires the
exact installed normal `Access` and its `Operation`, the GART creation generation,
`BothEnabled`, an actual ready firmware owner, the existing hardware hold, live
PCI D0/memory decode/BME and an absolute deadline at most one second away. The
same checks surround every read and selector write. A retirement guard cannot
substitute for normal authority. This does not introduce a new power-on or
GFXOFF policy; it requires the existing powered GC initialization path.

The immutable discovery must contain the supported GC11.0.0 and NBIF4.3.0
records. `GB_ADDR_CONFIG` is GC BASE0 plus DWORD offset `0x13de` and must fit
BAR5. The revision source is NBIF BASE2 plus DWORD offset `0x16`,
`RCC_STRAP0_RCC_DEV0_EPF0_STRAP0`; this is distinct from PCI revision `0xc8`.
The NBIF base is derived from `buildNbioLayout`'s verified BASE2 register
relationship, never a guessed absolute address. Discovery validation rejects
ambiguous records, unsupported versions, harvest/variant encodings, arithmetic
overflow and register/window aliases.

If the strap is outside BAR5, the capture uses the real RSMU sequence: read
INDEX_HI and require zero, record the attempted INDEX write, write the absolute
byte address, read INDEX to post the write, then read DATA. INDEX_HI is never
written. The posting read is ordering, not proof of equality or permission to
restore an old selector. Every write callback accepts only this private
strap/INDEX pair. No generic indirect-register endpoint is exposed.

Each snapshot brackets the two register observations with matching complete
PCI identity reads. The two complete snapshots must match full GB/strap and
PCI words, including bits not used by the color calculator. PCI words include
vendor/device, revision/class, subsystem, BAR4/BAR5, command and header type.
The native reader additionally matches BAR5's address/extent to its retained
map. All-ones reads, unsupported calculator topology, changes and any transport
failure stop immediately. There is no polling/retry or fallback topology.

An attempted capture is one-shot for the GART object. Private attempt/completion
counters precede the callbacks, so a failed selector write remains recorded as
possibly effective. Any failed attempted capture seals `FaultedRetained`, clears
firmware-ready admission and preserves the existing hardware/allocation holds.
No reset or retirement is inferred. The original creation generation, issuing
generation, status, trace and successful numeric snapshot stay private.
`copyColorTopology` provides a diagnostic numeric copy only while the same
normal generation/owner remains ready; callers cannot import a portable result
into that private record.

The next owner is VMContext: a color lease must retain the exact actual BO,
mapping, generation and byte range and prevent write/free/remap while held.
Retaining VMContext alone is insufficient. That binding and the later graphics
job are not implemented by this capture. The current record also does not
attest shader-engine/RB/CU harvesting or PA_SC tile-steering state, which the
full draw builder will need separately.

Source facts are pinned to Linux
`3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`: `gfx_v11_0.c`'s
GB_ADDR_CONFIG read, `gc_11_0_0_offset.h`, `nbio_v4_3.c:get_rev_id`,
`nbio_4_3_0_offset.h`, and `amdgpu_reg_access.c`'s RSMU accessors. The existing
`nbio.cpp` records the full protocol/license provenance. Address-profile
admission reuses the independently reviewed color validator documented in
`gfx11-color-target-design.md`.

Host tests exercise the actual portable parser/read protocol with controlled
input and transport failures; they do not pretend to observe hardware. Native
header compilation checks the real GART bridge, ABI and kernel frame limit.
The hash-bound report in `build/gfx11-color-topology-local` records exact source,
dependency, compiler, test and object identities. Native execution remains
pending a separately admitted target-engine session.
