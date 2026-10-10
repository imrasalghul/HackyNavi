# Native compute object ownership

`compute_context` connects persistent C++ resource objects to the actual v1.4
IOKit client. This is the backend for the forthcoming Metal object layer; it
does not register a Metal device or implement the Objective-C Metal protocols.

The current Metal object layer now uses this real copied I/O for Managed
storage. Each `HNMetalBuffer` owns its CPU copy and the actual native BO;
`didModifyRange` uploads the requested bytes and an ordered blit
`synchronizeResource` downloads actual native contents. A download publishes
no partial CPU result after a failed RPC. This is explicit Managed coherence;
the runtime still exposes no Shared pointer. `Context::allocatedSize` reports
actual persistent BO bytes under the same allocation/submission mutex and
fails on a closed owner rather than guessing its retained hardware memory.

The combined component layer compiled and linked in
`build/metal-managed-native-1` (eleven objects, 22 successful native commands,
three verified exports, stage removed). The public-Metal target smoke was
compiled only. Device publication, rendering and target execution remain
separate milestones.

`Pipeline::capture` copies a successful, actual `HNPipelineResult`, including
every code variant and active resource binding. It checks the native profile's
16 KiB code limit, wave32/register limits, resource masks and all canonical
exact-thread regions. It rejects whole-workgroup products and mismatched grids.
The copied code and metadata remain available after the original compiler
objects are freed. Kernel ISA decoding and admission still happen on each real
submission; these userspace checks grant no execution authority.

`Context::open` opens the real reusable native profile. `allocate` creates a
zeroed native allocation, while each `Buffer` retains the same connection and
BO generation across dispatches. Length checks use the logical byte length;
page padding is never exposed as extra application storage. `upload` and
`download` perform actual copied I/O. Download publishes its output only after
all chunks succeed. The C++ Buffer itself has no CPU mapping or implicit
upload; the Objective-C Managed layer above owns and synchronizes its CPU copy.

Each live allocation reserves a stable, distinct GPU virtual address in the
connection's isolated address space. The reservation survives dispatches; only
participating BOs are mapped. Confirmed native release makes a slot available
again. An address does not grant permission to submit pointers outside the
validated product interface.

`dispatch` pins all bound objects, reserves slot/byte headroom, uploads actual
immutable compiler code into unmapped staging BOs, and maps only the bound data
allocations. Exact resource subranges are preserved. Permissions come from the
union of real variant access masks. One BO cannot fill two bindings, and the
native profile requires at least one writable result. The method submits the
real product, confirms its native completion, releases CPU result history,
unmaps the data and releases staging. Persistent data BOs remain allocated;
using an output as a later input needs no CPU copy or reupload.

The implementation serializes all operations on a context. It reports
completion only after successful native job consumption and cleanup. Any
failed GPU-facing operation closes that context through the kernel's actual
retirement/recovery path, without retrying. `Buffer::release` and
`Context::close` expose errors; destructors provide fallback cleanup. Explicit
close invalidates all buffer references, while dropping only the Context
object lets live Buffer objects retain the real connection.

Native validation is in `build/native-compute-runtime-final/report.json`:
the actual C++ runtime and ARC Metal buffer object compiled and linked, and
233 offline checks passed under ASan/UBSan using real AIR translation and ACO
products. The tests cover independent code/metadata ownership, reordered
arguments, and all eight exact-thread regions. The 60,083,757-byte stage was
removed. No native connection was opened and no GPU command was submitted.
The later buffer context-identity correction passed a separate focused relink
in `build/native-compute-runtime-identity-final/report.json`, with the four
unchanged C++ objects independently verified byte-for-byte and the
40,960,697-byte stage removed. The earlier tests were not repeated. Do not
attribute that correction to the original immutable archive.

The target-only `hackynavi-context-smoke` is built, with its procedure in
[CONTEXT-SMOKE.md](CONTEXT-SMOKE.md). It requires actual A->B->C GPU dispatches,
retained B storage, stable addresses, output/guard checks and explicit cleanup.
It has not run on the Navi31 target. Metal discovery remains separate work.

`compute_shader` now retains an actual translated compute shader and builds
owned `Pipeline` products for the requested grid/local dimensions. Input AIR
and entry-name storage can be discarded after compilation. Each full/tail
variant goes through the real ACO compiler; its output remains owned after the
Shader is destroyed. Tool paths are supplied explicitly and compiler failures
preserve their diagnostics. This layer does not open a GPU.

Its local compiler-backed test passed 252 checks across five cases and fourteen
variants in `build/compute-shader-offline-local-2/report.json`. The comparisons
use independent calls to the actual compiler ABI and verify ownership after
source/result destruction. The preceding failed run exposed an exception for
an invalid compiler-path encoding; the corrected API now returns an error.
The same 252 checks passed under native macOS ASan/UBSan in
`build/native-compute-shader-library-final-2/report.json`. The Metal library/
function objects also compiled and linked in that run; no object or GPU
connection was instantiated. All exported artifacts were verified, and the
60,328,604-byte stage was removed.
