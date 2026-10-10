# Admission of immutable GFX1100 compute products

Current follow-on: actual one-thread ACO products now have explicit scalar
integer, lane-transfer and uniform MUBUF admission, plus packed local-ID
unpacking for multidimensional groups. Independent review, local checks,
220,693 native sanitizer checks and a real-header object compile pass. See
[the uniform-product implementation](gfx11-uniform-compute-admission.md).
The original instruction list below describes the earlier vector-offset
milestone and is extended by that record.

Latest admission update, 2026-10-11: the validator now enforces the register
occupancy of its fixed Navi31 wave32/WGP profile. Depending on the declared
VGPR allocation, the local-thread ceiling is 1,024, 896, 768 or 640. An
oversized group returns `InvalidLaunch` with derived launch/access output
cleared before decoding or publication. See
[the source derivation and current evidence](gfx11-compute-occupancy.md).
The focused native check passed 471,170 ASan/UBSan assertions and compiled the
changed kernel object; it did not produce or test a new complete kext.

Original product milestone, retained for provenance: the bounded leaf ISA validator, multi-region dispatch builder
and internal native product capture are implemented and frozen. Local sanitizer
checks, kernel-header object compilation and independent source reviews passed.
Native macOS sanitizer checks and the combined 91-unit kext link also passed;
all 406 imports resolve and both mini stages were removed. See
[the integration record](validation-product-admission.md). No
native GPU execution, Metal registration or rendering support is established
by that build. The subsequent v1.3 IOUserClient upload boundary is described
below and is now native-linked in `build/macos-product-wire-power-final`:
91 translation units, all 406 imports resolved and 233 verified sources.
Executable `c1cb8e7f402bc77c43682011e074ba2388f5f99cd673cc22f9f53104d1456aec`
predates the later VM reactivation/consume-job work. The native OFF-state
suites also completed with 11,554,469 capture and 50,298,906 retirement
checks, zero failures; both stages were removed. See
[the current integration record](validation-product-wire-power.md). The
artifact has not been loaded or executed on the target GPU.

## Implemented path and authority boundary

`userspace/metal/src/pipeline_product.rs` owns real compiler output and the
local-size variants needed for exact-thread dispatch. Its compiler ABI,
allocation metadata, reflection and SPIR-V checks remain frontend consistency
checks. A compiler name, JSON record, code hash or reflection mask cannot
authorize machine instructions supplied by that process.

Four implementation layers now connect that output to a bounded
internal native capture API:

| Layer | Implementation | Responsibility |
|---|---|---|
| ISA admission | `include/hackynavi/gfx11_compute_product.hpp`, `src/gfx11_compute_product.cpp` | Decode actual copied GFX1100 instructions; prove initialization, pending-load ordering, root/descriptor provenance and resource effects; derive launch registers. |
| Dispatch construction | `include/hackynavi/gfx11_dispatch_product.hpp`, `src/gfx11_dispatch_product.cpp` | Independently revalidate every variant, build exact full/tail regions, construct descriptors/payloads and canonical PM4, and compare immutable images. |
| Native ownership | `kext/HackyNavi/HackyNaviVMProduct.cpp`, `HackyNaviVMContext.hpp/.cpp` | Copy code into private owned DMA, resolve actual generation-bearing BO handles/mappings, exclude aliases, seal CPU edits and retain controls through the existing queue/completion/retirement lifecycle. |
| Userspace boundary | `native_abi.hpp`, `native_protocol.cpp`, `iokit_client.cpp`, `HackyNaviUserClient.cpp`, `HackyNaviClientSession.cpp` | Negotiate the explicit v1.3 profile, decode bounded copied messages, resolve connection-owned code staging and data handles, and dispatch through actual native capture and completion. |

The pure modules return diagnostics. Their public reports and numeric bindings
carry no native authority. `HackyNaviVMContext::captureProductCompute` invokes
the validators itself against retained private bytes and the real VM ledger.
Its pointer-bearing request remains an **internal kernel API**. The new
IOUserClient message contains buffer handles and offsets; Session constructs
the internal request from its own serialized, private CPU copies. No user
pointer is passed through as a kernel code pointer.

## Explicit v1.3 transport and session boundary

`ProductCompute` selector 13 requires the negotiated `ProductLeafCompute`
capability and minor 3. The older compiler-fixture and built-in profiles retain
their original contracts. The wire message is an 80-byte header/request followed
by up to eight 56-byte variants and thirty-two 32-byte resources, within the
existing 4096-byte message limit. Reserved fields, exact size, epoch, bounds,
alignment, duplicate/unused variants, duplicate resource bindings and code/data
aliasing are checked before native entry. The exact Cartesian full/tail
variant set is derived again from the grid and nominal local dimensions.

Session resolves every handle against its own generation ledger under its
mutex. Code staging BOs must be unmapped; it reads their actual bytes through
`VMContext::readBuffer` under the real FunctionGroup/GART scope. Up to 16 KiB per
variant is copied into heap-backed temporary input. Concurrent connection
writes, frees and mapping changes cannot change those copied bytes. Session
then reserves the existing aggregate result budget, seals the one-job
connection and calls `captureProductCompute`. That method independently
creates private code DMA and reruns ISA/resource validation. Temporary CPU
copies are freed after capture, before actual GPU submission.

The existing queue, completion reader and VM retirement path decide success.
Only successful actual completion/readback/retirement publishes a sequence;
failed or ambiguous work preserves the existing retained-DMA/recovery behavior.
A failed capture after sealing does not reopen the connection for reuse.
`Client::product` supplies actual epoch-bound handles, never GPU addresses,
PM4 or purported compiler authority.

Focused host validation passed 155,702 protocol cases, including 765 grids,
maximum-size messages, all truncations and single-bit mutations. Real x86_64
kernel-header compilation passed for Session and UserClient; an independent
review compiled these plus the decoder with a 2048-byte frame limit and found
no issue. Evidence: `build/product-wire-validation.json`,
`build/native-product-wire-local/kernel-compile.json` and
`build/product-wire-independent-review.json`. Native Session/UserClient/decoder
compilation, real IOKit tool linking and the combined c1cb8 build have since
completed as recorded above. None of these checks substitutes for a target
dispatch. This v1.3 connection remains single-job. The subsequent explicit
v1.4 profile implements reusable BOs and bounded historical results while
preserving v1.3; see [the reusable-resource record](compute-reusable-resources-design.md)
and `build/native-reusable-local/report.json`.

`HackyNaviVMContext` owns DMA allocations, private page tables and the VMID1..7
lease under the actual FunctionGroup/GART scope. The GPU VA range is
`[4096, 2^47)`. `vm_context.cpp` checks the root, disabled identity aperture,
context range, actual invalidation ACK and page-table publication. The older
fixed Apple `add_one` compiler class remains a separate class.

The queue remains **graphics PFP/ME ring0**, with an isolated-VMID indirect
buffer launching compute. Ring/writeback/final-fence storage stays in kernel
VMID0. This work neither creates an HSA/MEC queue nor imports its different IB
VALID/MQD/HQD/doorbell contracts. Relevant sources are Linux
`gfx_v11_0.c:5960-6080`, local `src/gfx_queue.cpp`, `src/vm_context.cpp` and
`kext/HackyNavi/HackyNaviGraphicsQueue.cpp`.

## Implemented Gfx1100LeafBufferV1 policy

The accepted class is straight-line wave32 integer buffer compute, bounded to
16 KiB of code per variant and at most 1,024 local threads, further limited by
register residency: declared VGPR counts 193–216 permit 896 threads,
217–240 permit 768, and 241–256 permit 640. These bounds require the fixed
Navi31 WGP/no-LDS/no-scratch/no-shared-VGPR profile. Every executable byte is
decoded. The final instruction must be `s_endpgm`; subsequent supplied code
bytes must be exact `s_code_end` padding. The private DMA allocation is rounded
to 4 KiB and its remaining bytes must be zero. Literal words are decoded as
instruction data, including literals whose bit pattern resembles a forbidden
opcode. No code hash or CPU replacement kernel decides acceptance.

The reviewed executable forms are `s_mov_b32`, `s_mul_i32`, `s_clause`,
`s_waitcnt`, `s_delay_alu`, `s_endpgm`, `s_load_dword`, `s_load_dwordx4`,
`v_lshl_add_u32`, `v_add_lshl_u32`, `v_add3_u32`, `v_mul_lo_u32`, `v_xad_u32`,
`v_lshlrev_b32`, `v_add_u32`, `buffer_load_dword` and `buffer_store_dword`.
`s_code_end` is padding only. Every unlisted opcode, unsupported modifier,
reserved field or extended encoding is rejected. There are no branches, calls,
scratch, LDS, images, atomics, traps, privileged-register instructions,
DPP/SDWA/VOPD forms or arbitrary PM4 inputs in this class.

The validator implements these dataflow requirements:

- Only SGPR0:1 (descriptor-table root), SGPR2:3 (48-byte payload root),
  SGPR4..7 (group IDs/TG size) and VGPR0 (packed local invocation ID) start
  initialized. Other registers start unknown. VCC, SCC, EXEC, m0 and temporary
  special-register selectors are excluded. Admitted instructions preserve
  EXEC, so vector initialization applies to the same CP-selected active lanes.
- SMEM reads require an intact original root pair and an exact bounded offset.
  The offset can include an SGPR proved constant by admitted scalar ALU.
  Descriptor reads must be aligned 16-byte loads within the 512-byte table;
  push reads must fit within 48 bytes. A descriptor load creates four tagged
  components for one binding. Any component write destroys that tuple; moves
  and arithmetic cannot manufacture or copy root/descriptor provenance.
- SMEM and VMEM destinations remain pending until the corresponding decoded
  GFX11 `lgkmcnt(0)` or `vmcnt(0)` wait. Partial waits do not prove readiness.
  Reads or overwrites of pending destinations fail. Counter tracking is
  bounded; delays never substitute for memory completion. Stores use the
  distinct VS counter and require the native queue's completion/visibility
  path; `s_waitcnt` is not claimed to retire them.
- Hard clauses are initially limited to at most 32 SMEM instructions, matching
  the pinned Mesa workaround. Count/class mismatches, pending RAW/WAW and
  replay-unsafe WAR dependencies are rejected. VMEM hard clauses are excluded.
- MUBUF operations require a complete live descriptor quartet and raw dword
  addressing with `offen=1`, `idxen=0`, no cache/LDS/TFE flags, zero immediate
  offset and a provably zero scalar offset. Integer ALU propagation proves
  the vector offset dword-aligned. The native descriptor base and byte length
  must also be dword-aligned. Actual load/store instructions derive read/write
  binding masks; no caller mask is accepted as authority.
- Physical register extents must fit the bounded requested allocation.
  GFX11 VALU constant-bus use is checked for at most two distinct SGPR/literal
  reads. Kernel RSRC fields are regenerated with fixed integer/no-scratch,
  no-LDS, no-shared-VGPR, nonprivileged wave32 WGP policy. The declared VGPR
  allocation is rounded first to its eight-register encoding, then to Navi31's
  physical 24-register granularity; the whole local group must fit the four
  SIMD register files. A larger allocation can reserve registers and lower
  the permitted group size; it cannot authorize a forbidden instruction.

The verifier uses no dynamic allocation or recursion. Its state is statically
bounded below 2 KiB, and code length bounds decoding work. On failure the report
contains only the status, instruction offset and word; derived access/launch
fields are cleared.

GFX11 raw descriptor bounds check `offset + payload` against NUM_RECORDS
(Mesa `ac_descriptors.c:815-838`). Keeping scalar offset zero avoids importing an
unproven alternative base/bounds interaction. The independent source audit
also checked applicable GFX11 instruction hazards. Ordinary `s_delay_alu`
omission remains correct by `aco_insert_delay_alu.cpp:17-29`; the separately
mandatory hazards involving wave64 masks, EXEC writes, transcendental
instructions, LDSDIR or WMMA do not occur in this admitted subset. This is a
source-reviewed bounded policy, not a proof that arbitrary GFX11 ISA is safe.

## Exact full/tail dispatch construction

The builder accepts up to eight distinct local-size variants, eight Cartesian
full/tail regions and 32 buffer bindings. It reconstructs regions from the
three-dimensional grid and nominal local dimensions. Missing or unused
variants, duplicate local sizes, duplicate resources, wrong permissions,
overlapping ranges and unused bindings fail admission.

For a 4,099-thread grid and nominal local size 64, it emits 64 groups of 64
using the actual local64 variant, followed by one group of three using the
actual local3 variant. The tail payload supplies thread base 4,096 and group
base 64. It does not round the tail up through the local64 shader. Every entry
point reruns ISA validation for every actual variant; no saved report bypasses
that check.

Each private parameter page contains a kernel-built 512-byte descriptor table
and one real 48-byte payload per region. Unused descriptor slots and unused
page bytes are zero. Data mapping permissions are exactly R or RW according
to the decoded effects; code and IB mappings are RX, parameters R. All BO IDs
and GPU mapping extents must be distinct. Each region uses kernel-generated
launch state and PM4, followed by CS_PARTIAL_FLUSH and a PFP ACQUIRE_MEM cache
writeback/invalidation sequence before later regions. The queue still owns
the final RELEASE_MEM fence, consumed RPTR observation and CPU visibility.

## Native capture, sealing and retirement

The native API copies request metadata into a private heap `ProductCapture`.
It reads each caller code image once into a fresh zeroed native DMA allocation,
then validates the copied bytes before mapping that code RX. Input pointers
are cleared after capture. Decoder metadata, resource arrays, workspace and
alias-check scratch stay off the kernel stack. No raw GPU addresses or user
PM4 enter the capture request: resources identify real owned handles plus BO
byte offsets/lengths.

The actual generation ledger resolves those handles. Every GPU mapping must
match exactly one admitted resource/control binding; extra aliases, extra
mapped BOs and mismatched permissions reject capture. All controls are made
internal before the serialized allocation/map path returns, including failure
paths. Captured state permanently denies public mapping edits and CPU writes.

Native alias checking covers **all resident BOs**, including unused unmapped
allocations, and every actual page-table DMA owner. It verifies ownership,
CPU geometry, segment geometry and complete segment coverage, then heapsorts
CPU and device-visible I/O intervals in bounded retained heap scratch. The
maximum scratch count is 17,410 entries including two CPU-only metadata
ranges. Overlap or duplicate ownership fails; the comparison cost is
O(N log N). These I/O bus addresses are not described as CPU physical
addresses. The allocator owns fresh IOBufferMemoryDescriptors and does not
support imported aliasing storage.

The completed private parameter image, actual shader DMA pages and canonical
IB are revalidated before capture succeeds and again in the existing queue
before VM publication. Unused IB words must remain zero. The selected result
handle and BO-relative byte range must lie within a binding actually written
by admitted ISA. Completion readback retains that exact range and generation.

Every partial failure after capture allocation becomes `FailedUnpublished`
and retains the context's private metadata/controls until the existing
pre-exposure cleanup consumes them. After ambiguous hardware exposure, the
existing registry/publication holds retain storage. Metadata release occurs
only through `releaseStorageLocked`, reached by CPU-only unpublished cleanup,
actual queue-proven completion plus VM detach, or the existing typed reset
retirement path. Real fence/RPTR/completion objects have not been replaced by
host-model success flags. These native paths are implemented but have not run
on the target GPU.

## Actual compiler products and validation evidence

All four retained products in
`userspace/compiler/fixtures/compute-products/products.json` pass the real
validator and dispatch tests:

| Kernel | Local size | Code / executable bytes | Code SHA-256 prefix |
|---|---|---|---|
| Apple-generated `add_one` AIR | 64×1×1 | 104 / 84 | `ac333acf99387a40` |
| Apple-generated `add_one` AIR | 3×1×1 | 104 / 84 | `98b773c473a79de6` |
| Authored `affine_xor` IR | 64×1×1 | 124 / 104 | `66bc66e2cb814db1` |
| Authored `affine_xor` IR | 3×1×1 | 124 / 104 | `0a9ad10b394dce95` |

The corpus records full hashes and compiler provenance. Each product came
through the owned pipeline C ABI and matched a separate ACO invocation. The
hashes identify evidence; they are not an acceptance list. Tests also admit
changed arithmetic literals and independently constructed valid programs.
The ACO IR dump is before final scheduling and is not treated as a machine-code
disassembly oracle.

| Evidence | Result and practical limit |
|---|---|
| `build/gfx11-compute-product-validation.json` | Local ASan/UBSan: **84,007 checks, zero failures**. Four real variants, forged roots/descriptors, uninitialized and pending operands, clauses, literal boundaries, forbidden encodings, reserved fields, bounds, counters, mutations and 20,000 random instruction streams. No GPU execution. |
| `build/gfx11-leaf-independent-review.json` | Independent read-only audit, no concrete findings, bound to exact validator/header/test hashes and pinned Mesa sources. |
| `build/native-product-review/review.json` | Pure dispatch suite: **120,732 checks, zero failures** under strict local C++11. Independent native capture review found no defects; it does not establish runtime IOKit/DMA behavior. Native Apple sanitizer runs remained pending at this report. |
| `build/native-product-review/intervals.cpp` and its review report | Exact extracted interval helper: **822,327 checks, zero failures** against an independent overlap oracle, exhaustive permutations through nine owners and maximum-size scratch cases. This checks the interval algorithm, not native DMA allocation/retirement. |
| `build/gfx11-leaf-native-local/kernel-compile.json` | Validator, VMContext and VMProduct compiled against the pinned real MacKernelSDK headers and were parsed as x86_64 macOS MH_OBJECT files. This is object compilation, not the combined kext link or a load/boot test. |
| `build/native-product-admission-final/report.json` | Six actual native kernel objects compiled; Apple ASan/UBSan passed 84,007 decoder, 120,732 dispatch and 22,512,972 display-retirement assertions, zero failures. All 61 source hashes matched and the stage was removed. No GPU execution. |
| `build/macos-product-admission-final/validation-summary.json` | Complete 91-unit development kext linked; all 406 imports resolved and all 233 source files verified. Stage removed. Deployment/ownership diagnostics retained; no kext load or target execution. |
| `build/gfx11-occupancy-admission-local/report.json` | Current occupancy integration: 85,839 strict and UBSan ISA checks, zero failures; targeted CMake ISA/helper tests passed. |
| `build/native-gfx11-occupancy-admission/report.json` | Current changed validator compiled with actual Kernel.framework headers. Apple ASan/UBSan: 85,839 ISA, 264,599 occupancy and 120,732 direct dispatch checks, zero failures. All ten source hashes matched; stage removed. This is one changed object, not a new full kext link or GPU test. Report SHA-256 `2e0733358d23b65acc830aafaa064cb7e6d0c53f8740f918232b751964224534`. |

Original full product-milestone source identities (historical, not the current worktree):

| File | SHA-256 |
|---|---|
| `src/gfx11_compute_product.cpp` | `79598e3a2b3f91b48e4c32c72647f159f4e9cafb5034f1f35ee8c5d6e1b96a00` |
| `src/gfx11_dispatch_product.cpp` | `2ef1783552e8359afca310174c51a1714d6679e6612e45821d47edfe04ee507d` |
| `kext/HackyNavi/HackyNaviVMProduct.cpp` | `95140537aa812375b210e6003b3b2e3a879e79c34ac4fe20185de1779b01aefb` |
| `kext/HackyNavi/HackyNaviVMContext.cpp` | `aa571c549da33d9e4ae2a65774e5bb36dde8c8310d1622a5da0c5f863dc72816` |
| `kext/HackyNavi/HackyNaviVMContext.hpp` | `9b401d9f9af06a7d29ac2dd0e61cbc07539c5712d6d9539be574d29ee8d414b8` |

Current occupancy integration identities are bound in its local/native reports:
validator `6937eac1196670560471f019b5d53187aa12194d5b8b4a9e25912b3e2c3a9cb0`,
helper `2f309f4ebd62fc90a4f45cdbb7faf870f12a5ba67c2f637afa19250859bb6749`.

## Source-derived decoder artifact

`userspace/compiler/generated/gfx1100-leaf-decoder-rows.json` is reproduced by
`userspace/compiler/generate-gfx11-decoder-rows.py --check`. It contains 18
observed opcode rows, exact instruction/operand/literal records for the four
retained products, source hashes and licensing provenance.
`capture-decoder-products.py` records the actual compiler output;
`test-decoder-rows.py` checks the structural decoder and its explicit limits.
The generated research table remains separate from the implemented C++
provenance/pending-load/native ownership checks. It does not grant admission.
The available local LLVM20 tool has no AMDGPU target, so no independent LLVM
disassembler result is claimed.

Pinned `aco_opcodes.py` / `aco_opcodes_cpp.py` supply generation-specific opcode
numbers and operand definitions. `aco_assembler.cpp` supplies physical field,
literal and extension layouts; `aco_ir.cpp::wait_imm::unpack` supplies GFX11
counter fields. The implementation preserves the GFX11 m0/SGPR_NULL swap,
rejects all unsupported special selectors, handles more-specific encodings
before broad scalar/vector containers, and checks the constant-bus restrictions
in `aco_validate.cpp`. Decoder expansion requires the same field, provenance,
initialization and asynchronous-dependency review for each added operation.

## The concrete VMID initialization fix

The original GC prepare path set `SH_MEM_CONFIG` for VMID0 only. The native
factory and portable context already admit only graphics VMID1..7, so those
contexts also need a known shader-memory configuration before publication.

`src/gfx_init.cpp` now selects each VMID1..7 through
`GRBM_GFX_CNTL.VMID` bits7:4, writes and reads back `SH_MEM_CONFIG=0xc00c`, and
writes and reads back `SH_MEM_BASES=0x20001000`. This is the pinned Linux graphics
policy: private aperture begins at `0x1000000000000000`, shared aperture at
`0x2000000000000000`, each 4 GiB; BASES contains their high16 address bits.
Neither can overlap an admitted low47 GPU-VA mapping. VMID0's existing config
is preserved and its bases are not changed; VMID8..15 shader-memory banks are
not touched. Selector0 is restored before continuing. The existing guard
requires all nonzero VM contexts disabled before any programming.

This does not allocate LDS or scratch, enable traps, activate a VM, or authorize
flat memory instructions. In particular, KFD's separate aperture and per-VM trap
policy must not be copied into the graphics VMID path. Existing GDS/GWS/OA
initialization already writes zero for every VMID1..15; that was not a missing
feature.

Source: Linux `gfx_v11_0.c:336-339,2108-2169,2213-2237`,
`gmc_v11_0.c:620-635`, `soc21.c:soc21_grbm_select`, and generated
`gc_11_0_0_offset.h:6204-6205,6282-6285` /
`gc_11_0_0_sh_mask.h:24044-24054,24671-24684`.

Validation: `build/gfx-init-shmem-validation.json` contains exact commands and
file hashes. ASan/UBSan: 17,403 initialization checks and 5,156 queue checks,
zero failures. The bank-aware model checks dirty initial state in all16 VMIDs,
every new selector/config/base access failure (including landed false writes),
every corrupted/all-ones post-write read, final selector restoration, and no
later CSIB/SRM publication after failure. The changed source compiles as an
x86_64 macOS kernel object against the pinned real MacKernelSDK headers. Full
Apple SDK integration build and real GPU execution are separate evidence.

## Remaining integration and target evidence

The original combined Apple-toolchain kernel build/link and native sanitizer
checks are complete; see [validation](validation-product-admission.md).
The later occupancy change has focused object/sanitizer evidence as recorded
above and has not been linked into a newly validated complete kext here.
The current evidence must not be described as a booted or accelerated macOS driver.

The next implementation steps are:

1. Validate the implemented v1.3/v1.4 product boundary and owned runtime on the
   target GPU. Session already copies code, enforces connection generations
   and budgets, and resolves real handles before the private native API.
2. Run the already native-linked product/context smoke tools with real
   full/tail variants, result/guard-byte verification, genuine completion and
   verified retirement. The retained A-to-B-to-C resource path and historical
   result release remain target evidence to collect. No CPU fallback or
   Windows-driver execution establishes this macOS path.
3. Complete target runtime fault attribution and demonstrated admission
   closure, drain/reset and retained-owner retirement for VM faults, illegal
   instructions and hangs. VM fault-enable bits and host fault models do not
   implement or prove IH service, private-register/private-instruction/bad-op
   handling or target recovery.
4. Establish the target engine/ownership prerequisites, then perform a native
   GPU run and record the actual fence/RPTR, output and retirement evidence.
   These captures must remain separate from diagnostic-only display snapshots.
5. Implement Metal registration and its real device/queue/resource/frontend
   integration. Render pipelines, IOSurface/presentation, WindowServer and
   display modesetting remain separate incomplete driver work.

For unrestricted compute, additionally prove complete nonprivileged context
policy, special address/aperture behavior, register/LDS isolation, traps,
scratch, watchdog/preemption and recovery for every admitted engine. A VM page
switch alone does not sanitize previous register state. Linux has separate
`enforce_isolation` / `run_cleaner_shader` policy (`amdgpu_cs.c:298-316`).
A dedicated MEC owner would require real MQD/HQD/doorbell ownership and teardown;
it would not remove these instruction, state-isolation or recovery obligations.

## Pinned primary references

- Linux commit `3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`:
  [GC11 initialization and queue implementation](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c),
  [GMC11 apertures/PTEs](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu/gmc_v11_0.c),
  [command ownership and isolation policy](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu/amdgpu_cs.c).
  Local files are under `.research/linux`.
- Mesa commit `35b085c4c067f198ce0b6437d94678e47b9c645f`:
  [ACO opcode data](https://gitlab.freedesktop.org/mesa/mesa/-/blob/35b085c4c067f198ce0b6437d94678e47b9c645f/src/amd/compiler/aco_opcodes.py),
  [ACO assembler](https://gitlab.freedesktop.org/mesa/mesa/-/blob/35b085c4c067f198ce0b6437d94678e47b9c645f/src/amd/compiler/aco_assembler.cpp),
  [GC11 register fields](https://gitlab.freedesktop.org/mesa/mesa/-/blob/35b085c4c067f198ce0b6437d94678e47b9c645f/src/amd/registers/gfx11.json).
  Local files are under `.research/mesa`; this audit used those pinned sources.
- Cobalt64 commit `d6c9aedf8e6526a39e349b5886cbc8c7ddf77f65`:
  [gfx_admit112.h](https://github.com/Almosst-DEV/Cobalt64/blob/d6c9aedf8e6526a39e349b5886cbc8c7ddf77f65/src/navi48-bringup/src/apple/gfx_admit112.h)
  explicitly limits its admission policy to six inspected programs and real
  frame/VM provenance. It is a different ASIC/path and supplies no general
  GFX1100 machine-code verifier. Its hash scheme is not imported as authority.
