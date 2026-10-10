# Executing the recorded Apple add_one ACO product

The design below is now implemented in source as a separate sealed native
class and explicit v1.2 transport. Focused native compilation and sanitizer
checks pass; combined-build evidence is recorded separately. The v1.1 profile
remains one-job clear/copy. Neither the new ACO class nor the clear/copy client
has run on the target. Windows Vulkan results used the Windows driver's
compiler, not these ACO machine-code bytes.

## Implementation map

`userspace/compiler/fixtures/apple-add-one` preserves the original AIR, SPIR-V,
104-byte ACO product, launch metadata and source/license manifests.
`verify_fixture.py --check` pins the manifest digest and exact generated header;
native/cross/host builds require this check. Remote native builds carry the
small fixture and verify it again on mini.

`src/gfx_aco_fixture.cpp` builds the real descriptors, 48-byte dispatch payload
and wave32 PM4. `HackyNaviVMContext::captureCompilerCompute` resolves actual
owned mappings, allocates the three private controls and seals them. The
queue validates the captured class and uses its existing GPU fence/RPTR;
result identity is independent of clear/copy's request layout. Session keeps
one actual FunctionGroup/GART scope through capture, submit, readback and
completed-VM retirement. It publishes a result sequence only after all succeed.

`nativeabi::Selector::CompilerCompute` is selector 12. Explicit minor 2 and
`kCompilerCapabilities` negotiate only `AppleAddOneGfx1100V1`; arbitrary shader
upload, raw PM4 and CPU mapping remain unavailable. The transport's
`Client::openCompiler`/`compilerAddOne` and target-only
`hackynavi-compiler-smoke --run` check the real result and untouched guards.
Compiled tools live in `build/native-transport-compiler-v12/artifacts.tar.gz`.
They were not executed on mini or the target. The pending diagnostic capture
package does not contain or enable this engine path.

Focused evidence: `build/native-gfx-aco-fixture/report.json` (81,217 sanitizer
checks), `build/native-compiler-vm-final/report.json` (native VM/queue objects),
`build/native-userclient-compiler-v12/report.json` (native Session/UC/protocol),
and `build/native-transport-compiler-v12/report.json` (135,829 protocol checks
and all four native tool/library outputs). All temporary mini stages were
removed. The earlier VM regression run also passed 6,965 isolated-compute and
5,156 queue checks; it does not simulate native allocator failures.

## Smallest complete implementation

Add a separate, explicitly negotiated `AppleAddOneGfx1100V1` sealed shader
class. At build time, package the exact already-produced shader plus its checked
launch contract into a private kernel constant. At runtime accept an operation
ID and connection-owned source/destination handles, never user machine code,
PM4, descriptors, register values, or arbitrary GPU pointers. This executes the
actual compiler product while keeping general shader admission out of scope.
Keep the existing clear/copy encoder, validation and ABI behavior unchanged.

The first case is 4,096 uint32 elements, local size 64x1x1, 64x1x1 groups, one
captured IB and one VM. Use the current real FunctionGroup Operation, GART
Access, queue submission, GPU-written fence/RPTR, DMA readback and per-context
retirement. No additional queue or invented completion mechanism is needed.

## Exact retained product and launch contract

`build/gfx11-compiler/apple-add-one-20261010-195643/` is identified by the actual
`test-report.json`. Its `shader.bin` SHA-256 was rechecked during this review:
`ac333acf99387a4063fae75a89dd4e6968907f6b78702d26442dd8a3575f7dda`.
Keep all 104 bytes, including the padding; executable bytes are 84. The SPIR-V
hash is `dc617a376ba9594f26dfb3b07656ef4a87236c5fb1ff387b02d0fdb8a59e2439`.
The ACO IR shows descriptor loads, buffer load, add-one, buffer store and
`s_endpgm`; it is not a CPU implementation of the result.

| Field | Required value |
|---|---|
| Mesa / target | `35b085c4c067f198ce0b6437d94678e47b9c645f`, `gfx1100` |
| Wave / workgroup | 32 / 64x1x1 |
| RSRC1 / RSRC2 / RSRC3 | `0x202c0002` / `0x788` / `0` |
| Allocation metadata | 24 VGPRs, 108 SGPRs, no shared VGPRs, scratch, LDS, spills, constants or relocations |
| Descriptor bindings | Set 0, mask 3: binding 0 input, binding 1 output |
| User SGPRs | 0-1 descriptor-table GPU VA; 2-3 push-payload GPU VA |
| System inputs | SGPR4-6 workgroup IDs; SGPR7 TG size; VGPR0 packed local ID |
| Dispatch | Direct group counts 64,1,1; zero COMPUTE_START; wave32 bit `0x8000` plus legal compute enable |

Do not substitute existing `gfxcompute::encode` values: those launch different
libdrm shaders with different descriptors and wave/resource settings. Preserve
the recorded WGP mode, rather than clearing its bit as the libdrm preset does.
Use the current GC11 preamble/cache invalidation and trusted register whitelist,
then write the recorded PGM/RSRC registers, NUM_THREAD=64,1,1 and four user SGPRs.
RSRC3 stays zero; no instruction prefetch is authorized by this contract.
Keep the CS drain/PFP synchronization and queue-owned cache-writeback fence.
Source the exact initiator fields from pinned GC11 PM4/Mesa definitions; the
existing direct enable plus wave32 produces `0x8001` with zero START state.

## Private memory and result identity

Extend captured-job representation with an explicit shader-class discriminator
and result range; do not overload clear/copy's `bytes` as a thread count.
Resolve all handles against this context's retained generation slots and actual
mapping ledger under its existing lock. Limit all VAs to the existing lower
47-bit window, even though the offline descriptor helper permits 48 bits.

Allocate three private 4 KiB controls: code RX, IB RX, and descriptor/payload R.
The latter contains two 16-byte raw descriptors at offsets 0/16 and a 48-byte
push payload at aligned offset 32. Never export these handles or allow any
writable alias. Code is at least 256-byte aligned. Copy the full recorded code,
initialize the remaining allocation, construct descriptors from actual retained
source/destination mappings, and seal CPU/mapping mutation before exposure.
All three controls must be distinct from data BOs, including physical BO aliases.

The descriptor builder must match pinned Mesa `ac_build_raw_buffer_descriptor`
for GFX11: stride zero, byte count (not bytes/16), RAW OOB selection and resource
level false. The verified C helper emits word3 `805400492` (`0x30016fac`), with
word0/1 the GPU VA and word2 the byte limit. Implement this narrow builder in
the portable kernel-safe encoder and compare it against the existing real Mesa
helper; do not import userspace Mesa into the kext or trust user descriptors.
Input maps R; output maps RW for the current readback contract.

The actual translator planner at `userspace/metal/vendor/metal2vulkan/src/reflect/mod.rs`
defines the 12 little-endian u32 words. For this first exact-thread region they
are `[4096,1,1, 0,0,0, 0,0,0, 64,1,1]`: grid, thread base, threadgroup base,
total threadgroups. The fixture's ACO currently reads word3, so leaving the
payload absent because its base is zero would still be wrong.

For meaningful testing, allocate 20 KiB input/output BOs and preserve 64 poison
guard words after the 4,096-element grid. Give the fixed fixture descriptors a
16,640-byte data/guard range and bind that same result extent to completion,
while dispatching only 4,096 threads. This detects over-dispatch instead of
hiding it behind a smaller descriptor bound. Separately check active input
length and output capacity; check all additions/multiplications for overflow.

`captureValidLocked()` must recheck each role, immutable controls, exact mapping
and generation, copied shader class and canonical PM4 before publication. The
queue's completion currently derives its result from `captured_.dispatch`; make
that a class-independent retained output range. Preserve exact context/epoch/
VMID/producer/sequence checks, raw-IB exclusion and sole-producer evidence.
After the genuine fence and RPTR, synchronize the output, copy all result/guard
bytes under the same Operation, then call `completion->retireContext()` before
exposing the immutable CPU result. Use the existing 64 MiB/64-holder result cap.
Ambiguous/in-flight jobs remain retained for real retirement; never free them
because a compiler, command, or timeout returned an error.

## Missing integration metadata and larger boundary

The recorded `shader.json` already contains the hardware launch configuration;
there is no need to guess RSRC values. It does **not** bind all of the following
into a single immutable executable contract:

- Original AIR and translator/reflection hashes, Metal `add_one` and SPIR-V
  `main`, reflection version 56 and descriptor-layout version 1.
- Per-binding access and footprint. Actual reflection gives input ReadOnly,
  output WriteOnly, each `4 * GlobalInvocationIdX` with a 4-byte access.
- Dispatch kind `ThreadsDynamic { offset: 0 }`, specialization IDs 0,1,2,
  exact grid/region payload and the code variant for each region.
- A kernel-admissible provenance/class identifier. File hashes and user JSON
  are consistency evidence, not authority to run arbitrary supplied code.

The build-time packer should require these artifacts together, reject any
mismatch/unsupported field, and emit a source manifest plus the fixed class.
Store the fixture/code in a tracked location rather than requiring an ignored
build directory at every future build. No replacement code may be synthesized
if artifacts are missing. A later dynamic compiler service needs a versioned
pipeline product ABI, private capture, bounded compiler-process execution, and
explicit shader admission/fault policy. SPIRV-Tools validation does not itself
prove arbitrary machine code safe or bounded.

The 4,099-thread test needs two planner regions: local64/groups64/base0 and
local3/groups1/thread-base4096/threadgroup-base64; total groups are65. The
current compiler tests compile local3 only in a removed temporary directory.
Retain and hash that actual variant before admitting it. Never round up the
existing local64 shader and pretend it implements the translator's tail
semantics. Exposing the existing Rust planner through a bounded C result API
is the next frontend bridge; the current C header exposes reflection only.

## Suggested implementation ownership and validation

1. Compiler lane: exact-product packaging/manifest and, later, planner C ABI.
2. Portable submission lane: new `gfx_aco_fixture.hpp/.cpp`, immutable payload/
   descriptor builder, canonical PM4 validation and negative admission tests.
3. Native VM/queue lane: new capture method and typed class, third control BO,
   class-independent result binding; reuse activation/publication/retirement.
4. Transport lane: a separate capability and selector/minor-version path,
   session adapter, and explicit `--run` native fixture tool. Existing v1.1
   clear/copy negotiation must continue rejecting this new class.

Host checks should target wrong byte/metadata hashes, descriptor size/stride,
wave64 or altered RSRC, malformed payload/local dimensions, added PM4, aliases,
foreign handles, every partial-allocation failure and completion identity.
Then native-header compile/link. A later authorized target run must compare
every GPU result to input+1 (including uint32 wrap), preserve every guard word,
confirm real completion and successful context retirement/close, and record
binary/contract/kext hashes. CPU validation is only an oracle; it must never
populate the reported GPU result on failure.

## Reference boundaries

Pinned Mesa `src/amd/common/ac_descriptors.c:797,860,880` supplies raw descriptors;
`src/amd/vulkan/radv_cmd_buffer.c:15158` separates group counts, thread dimensions,
START and wave32. Our compiler's `emit_binary` and `lower_resources` are the
authoritative ABI for this product, not RADV's different pipeline-layout ABI.

NVIDIA's Metal/Vulkan device and encoder architecture and Cobalt's native
per-VM code/IB path are architectural references. Cobalt's Navi48 register
values and its LLVM AMDHSA kernarg ABI do not apply to this GFX11 ACO product.
Concrete local references are Cobalt `src/navi48-bringup/src/amd/compute_test.cpp:1001`
and `tools/native/navi48metal/Navi48Device.m:5750`. Avoid its placeholder-pipeline
skip path at line5721. NVIDIA publishes the separate plugin contract in
`kexts/NVRM/accel/Info.plist:35` and implements the device subclass/backend in
`plugin/NVMTLDevice.m:344`. RADV normally packs descriptor pointers into one
SGPR with `address32_hi`; **our compiler explicitly uses two SGPRs and a full
64-bit pointer**, so RADV's pointer packing must not be transplanted here.
Executing this fixture still does not register an `MTLDevice`, implement Metal
Objective-C resources/encoders, expose IOSurface presentation, or accelerate
WindowServer. Those are distinct frontend/display integrations.
