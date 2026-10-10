# Native compiler-product transport and current display power state

Updated 2026-10-11. The v1.3 product interface now connects actual uploaded
GFX1100 code to the existing native admission, queue, completion and retirement
path. The display startup/idle/power protocols now interpret current PGFSM
state consistently with the source definitions and new target observations.
This record distinguishes the current development code from the separate
frozen capture package. No target GPU execution or Metal device is established.

## Implemented changes

- Product selector 13 negotiates an explicit minor-3 capability. Its copied
  message contains generation-bearing staging/data handles, bounded code
  extents, local-size variants, grid dimensions and a result range. No GPU
  pointer, descriptor, PM4 or compiler-supplied validation report is trusted.
- The real Session mutex serializes code copies from unmapped staging BOs
  against upload/free/map operations. The native capture independently
  allocates immutable controls, decodes actual ISA and resolves actual VM
  mappings. Real GPU completion, result readback and context retirement decide
  success. Ambiguous exposed work follows the existing retained-owner failure
  path; no result is computed on the CPU to replace failed GPU work.
- The actual IOKit client supports this profile. The target-only
  `hackynavi-product-smoke` loads the real compiler runtime and ACO tool,
  compiles supplied AIR/IR into full and tail variants, uploads those actual
  bytes and submits 4,099 threads. It requires all returned values, 1,021
  unchanged suffix guards and successful explicit close. Its CPU calculation
  only compares GPU readback. The native build script never executes it.
- `cold_dma`, `display_idle` and `display_power` classify raw STATUS0 or
  `0x10000000` with CONFIG0/1 as current ON, and raw `0x80000000` or
  `0x90000000` with CONFIG `0x100` as current OFF. Unknown bits, conflicting
  requests and changes during stable observation reject. All actual idle,
  ownership, clock and retirement requirements remain. See the
  [source audit](display-power-current-state.md).

The frozen connection still owns one job. Reusable Metal resources, multi-command
buffer scheduling, Objective-C Metal objects, normal Metal discovery, rendering
and presentation remain separate work. This interface does not advertise those
features.

## Completed checks

`build/product-wire-validation.json` records 155,702 strict C++11 protocol
checks, including 765 grids, maximum payloads, truncations, mutations and
profile/alias rejection. `build/product-wire-independent-review.json` records
an independent boundary review and kernel compilation of Session, UserClient
and decoder with a 2,048-byte stack-frame limit. No defect was found.

`build/product-smoke-validation.json` records actual x86_64 macOS 15 object
cross-compilation. `build/product-smoke-independent-review.json` checks the
real C ABI/Rust contracts, actual code flow, result/guard/close behavior and
build-stage lifetime. Native tool linking and export are now completed below.

`build/native-product-wire-power-final` passed native macOS ASan/UBSan:

| Suite | Checks | Failures |
| --- | ---: | ---: |
| Cold DMA | 1,146,247 | 0 |
| Display idle | 328,847 | 0 |
| Display power | 1,353,761 | 0 |

Session, UserClient, the protocol decoder and all three power consumers
compiled into actual x86_64 kernel objects. All 401,885 staged source bytes
matched the worktree after export. The 2,544 KiB temporary stage was removed.
The three power consumers also passed independent source review and local
UBSan checks. These register-model tests do not exercise actual GPU power.

Fresh CMake integration in `build/cmake-product-power` passed all four selected
suites: native ABI, cold DMA, display idle and display power. All recorded
input hashes matched. Initial Windows archive-tool configuration errors and
their correction are preserved alongside the final successful report; they
were build-tool setup failures, not failed driver assertions.

## Native IOKit build

`build/native-transport-product-v13` successfully compiled/linked the real
IOKit dylib and all four tools on Intel macOS 15.8 / SDK 26.2. The native ABI
ASan/UBSan suite passed 155,702 checks, zero failures. All 148,998 staged source
bytes matched after export. All five exported binaries were verified as
x86_64 Mach-O, and the 944 KiB stage was removed. Neither probe nor any GPU
smoke tool was executed on mini.

- Transport dylib SHA-256: `230bc316d9622c9061dd4c5de0e081f73ec10e84a52314ed0b1a9ce4f9c2abbf`.
- Product smoke SHA-256: `aab6d2b97a2f5b63fc7301cdb7e3575c62e1ef17c7c25130c9b21b113ef89955`.
- Source archive SHA-256: `64e085cfcb7130041bea860a769a2f53a86207df5edc1a983b633a7c5675b585`.
- Artifacts archive SHA-256: `613a19cce5f5cf331aaaf0ee869db75f4a549ff6deb178f8066ecec837802c89`.

## Native boot-display OFF-state checks

The later development-only boot-display OFF-state correction accepts either
desired-state bit when the actual PGFSM state is OFF (`0x80000000` or
`0x90000000`), with CONFIG `0x100`. Exact raw stability and window-access
guards remain. The Device raw snapshot excludes both OFF forms from window
reads, including incomplete captures. Independent source review and local
strict/UBSan checks passed.

`build/native-display-off-state-final` completed native macOS ASan/UBSan:
11,554,469 capture assertions and 50,298,906 retirement checks, zero failures.
Device, boot-display capture and boot-display retirement compiled as real
x86_64 kernel objects. All 504,044 staged source bytes matched at completion;
the 2,532 KiB stage was removed. Evidence is in its `report.json` and `run.log`,
with the separate review in `build/boot-display-off-current-independent-review.json`.

## Preceding frozen product-wire/power build

`build/macos-product-wire-power-final` completed the combined native build:
91 translation units, all 406 kernel imports resolved, zero unresolved symbols
and all 233 archived source files verified. The uncompressed source snapshot
is 4,892,385 bytes; the executable is 3,042,448 bytes. The 9,724 KiB temporary
stage was removed after export. `build-report.json` and
`post-export-validation.json` bind the exact inputs, executable and cleanup.

- Executable SHA-256: `c1cb8e7f402bc77c43682011e074ba2388f5f99cd673cc22f9f53104d1456aec`.
- Mach-O UUID: `951E2F2C-F37D-3B85-ABE4-A348BC9998DE`.
- Source archive SHA-256: `6707987f7dab900358c62f0aecc6c587624c8cef7ac00ce8256813b5ce422809`.
- Artifact archive SHA-256: `9a3114ee58b1f93df3b2d74772f64ef987b95fd6d503966320fc7d94879f6ff7`.
- Build report SHA-256: `b3f1e20a6b1916122f7c53d1174d82a9d676f8a88fa9d584211d54a4abc97b40`.

Both no-load diagnostic commands exited zero. Their logs still record staged
ownership `501:20` versus required `0:0`, and the linker warns that SDK 26.2
kmod runtime objects are newer than the macOS 15 deployment target. These
checks do not establish target load compatibility. No kext was loaded and no
GPU work was executed during this milestone.

This immutable c1cb8 artifact predates the checked VM reactivation and native
consume-job work. That work began after its source archive was fixed; the
export verifier records the two subsequent portable VM-context source/header
differences. Later mutable source needs its own review and validation. The
build proves native integration of the v1.3 product boundary and power fixes,
not reusable resources or a working Metal device.

## Completed native resource-reuse milestone

`build/native-reuse-final` compiled eight actual kernel objects: VMContext,
VMProduct, VMCompletion, GraphicsQueue, GraphicsQueueReuse, EngineController
and the portable VM-context/queue implementations. Strict compilation included
`-Wframe-larger-than=2048`. Native ASan/UBSan passed 241,988 VM-context and
5,202 queue checks, zero failures. All 807,856 source bytes matched at
completion, and the 3,820 KiB stage was removed. Independent ownership and
probe reviews are recorded alongside the focused validation report.

The full `build/macos-reuse-final` build includes 92 translation units,
406 resolved imports, zero unresolved symbols and 234 verified source files.
Its source snapshot is 4,926,483 bytes; the executable is 3,055,968 bytes.
All six exported bundle files matched the artifact archive. The 9,792 KiB
remote stage was removed. `post-export-validation.json` verifies the archive
against the frozen manifest, rather than relying on later mutable sources.

- Executable SHA-256: `fe01c5be0f8838dd238fd079cf7709324a81a69d13254e9590f5c50ec854f66b`.
- Mach-O UUID: `62487AB0-90FD-3668-B0D3-0DE389D35EEA`.
- Source archive SHA-256: `46544da0d272eef38b8b8082e0cba5c206464a49671e7c07331b920bd9888df1`.
- Artifact archive SHA-256: `9c9cb27d6c8507bf909044ebe2bf45cccb4f1b2fcad608607e33316ec071a67c`.
- Build report SHA-256: `95de4528cbba3146f2a9ba32297fd3f1857cf89fd3543b86941a42fbe434122d`.

This milestone adds checked detached-VM reactivation and consumes actual
queue-issued completions before releasing private controls while preserving
data owners, root, VMID and generations. The new `ReusableVerify` stage follows
`ShaderVerify` under the existing engine opt-in. Its target-only A->B->C probe
requires two actual GPU fences, 8,320 compared words including 128 guards,
nine stale-completion rejections and three reused control slots with newer
generations. It never uploads B's CPU readback. These are the probe's required
outcomes, not observed GPU results: it has not executed on the target.

Both no-load diagnostic commands exited zero, with the same staged ownership
and newer-SDK runtime warnings described above. No load compatibility or GPU
execution claim follows from the link. The subsequent v1.4 Session/client
profile and bounded result history are implemented outside this frozen build.
Its supplied-source `--reuse` smoke cross-compiles and passes independent
review against the actual client APIs; native transport linking is recorded
below and target invocation remains unverified. Evidence:
`build/product-smoke-reuse-validation.json` and
`build/product-smoke-reuse-independent-review.json`.

## Reviewed v1.4 reusable Session and client source

The later v1.4 source explicitly negotiates `ReusableCompute`, preserves the
old single-job profiles and retains up to eight CPU result copies identified
by connection epoch and actual queue sequence. A successful native job must
complete, read back and consume its private controls before the Session
returns to reusable idle. The client checks table capacity before submission;
released fences return `NotFound` without an RPC, output write or mutation.
An ambiguous later submission stops new work while preserving older history.
Explicit close also covers the reusable profile.

`build/native-reusable-local/report.json` binds the frozen implementation and
records 158,417 strict protocol checks plus 158,417 UBSan checks, zero failures.
Session, UserClient and protocol objects compile against real kernel headers.
Separate Session/UserClient and transport reviews found no concrete defect:
`build/native-reusable-local/session-independent-review.json` and
`build/native-reusable-local/transport-independent-review.json`.
The latter report SHA-256 is
`a2cb9d51898c997c6cc53f113e8a3aeb25d14c465cf9c64831ff14bd43aab15e`.
It also verifies that `--reuse` uses the same retained B BO for B->C and that
releasing the first historical result leaves the second readable.

The native transport build in `build/native-transport-reusable-v14` subsequently
linked the real x86_64 IOKit dylib, probe and all three smoke tools, including
the supplied-source `--reuse` path. Native ASan/UBSan passed 158,417 protocol
checks, zero failures. All five artifacts were hash-verified and exported,
source hashes matched, and the 1,040 KiB stage
`/Users/victor/Library/Caches/HackyNavi-transport.E21Leb` was removed.

- Native report SHA-256: `c91d4880c2d1d13d8282d643bedb317dd5af2299cc51881dab785710cd3cfa74`.
- Source archive SHA-256: `1ea9aa5c749e4bb63b14b2a2a05c87862962195f38d882dc059d772eb44ab2f1`.
- Transport dylib SHA-256: `94ea9dbc73ae48e66c31384bd837cc8b65d4e1834af30ba7c600952ce1fffa93`.
- Product smoke SHA-256: `46c151437fe3e8d444fba1d145e804c4dc1cafc643c5dcff8fada8c92be9c185`.

The tools were not executed against a driver. The combined v1.4 kext snapshot
is complete as recorded below. No target GPU execution, repeated-job recovery
or Metal device is established by these source reviews and protocol checks. The fe01c5be
artifact above predates this v1.4 source; its identity and diagnostic scope
remain unchanged.

## Completed combined v1.4 native kext

`build/native-reusable-session-v14` compiles the actual Session, UserClient
and protocol decoder with the 2,048-byte frame limit. Source hashes match,
all commands exit zero and the 616 KiB stage was removed. This check contains
three kernel objects, not a native Session/GPU execution test.

`build/macos-reusable-v14-final` links all 92 translation units and resolves
all 406 imports. Its 234 source files total 4,935,418 bytes; the archive is
1,501,490 bytes and the executable is 3,060,568 bytes. The frozen manifest
and all six exported bundle files were verified. The 9,808 KiB stage
`/Users/victor/Library/Caches/HackyNavi-build.2BXrTe` was removed.

- Executable SHA-256: `67fc0db187d72a5fd459194924d0f17b91dd6f20c3a08c69eee6d19863ea8cb6`.
- Mach-O UUID: `57C5BBE4-4867-34FA-B4E6-1E3F7F6656CB`.
- Source archive SHA-256: `8b207295179bd8f4fd20ae5ec6ba2e9a26c66266f09b69fa2affd28c31253889`.
- Artifact archive SHA-256: `e0c4c5c7a2f190aa03134f7cb12bcd11b66b12dfdcfa8559fc8c8a145db1d831`.
- Build report SHA-256: `adddb984c6caff3b209c30c0d0a9c96598206b875796110e1184d3b01ffeb1a7`.
- Post-export validation SHA-256: `57b37e108da499aa08e3dfded4916ecd271bdf2b89330617cc10b47f26dd1387`.

No-load diagnostics exited zero and reported the recorded staged ownership
mismatch (501:20 versus 0:0); this does not establish target load compatibility.
The artifact has not been loaded or executed on the 7900 XTX. It includes
v1.4 native resource/result ownership; later userspace C++ runtime work remains
separate from this source archive.

## Target-only compute Context test

`userspace/runtime/context-smoke.cpp` uses actual production `Context`,
`Buffer` and immutable `Pipeline` objects. It compiles supplied AIR/IR with
the real compiler C ABI/ACO, frees the original compiler handles and verifies
the copied code. All A/B/C data uploads precede the first dispatch. It checks
A->B then the same retained B->C, both outputs and guards, unchanged B and
stable addresses, matching epochs/increasing sequences, released-buffer
rejection and explicit native cleanup. `--run` is required.

Strict x86_64 macOS 15 object compilation passes, recorded in
`build/context-smoke-local/report.json`. The later native runtime build links
this tool without executing it; the test has not run on the GPU. See
[the target instructions](../userspace/runtime/CONTEXT-SMOKE.md). Neither the
wrapper nor its test implements normal Metal discovery or graphics objects.

## Native compute runtime and private buffer compilation

`build/native-compute-runtime-final/report.json` records strict x86_64 native
compilation/linking of the actual C++ runtime and ARC `HNMetalBuffer`, plus
233 actual compiler-backed ASan/UBSan checks. The target Context tool is
compiled/linked only; no Context connection or GPU test was opened. The
actual SDK26.2 `MTLAllocation.h` was exported and requires `allocatedSize`,
which the object implements. No substitute Metal device was instantiated.

- Initial report SHA-256: `06e0bbd48180665cb8e490f6dfb8d3cf1d141a2168b660ec77a5153556ea160e`.
- Initial source archive SHA-256: `44a9fd78dea6575ecb5767e2262967849f528766c73a618e85270074fabade52`.
- Initial runtime dylib SHA-256: `11a4275bb0b3589d5ab3cccb5572ab1cd8fa4cadfd09930f7b29f72a68214b4a`.
- Target tool SHA-256: `91efa123f614dd18becd534a90b9fd6ce75517676ae3a84ac565593f658def1c`.

All six artifacts were exported and hash-verified, the source matched at
completion and `/Users/victor/Library/Caches/HackyNavi-ComputeRuntime.sqVXBG`
was removed after reaching 60,083,757 observed bytes.

The subsequent exact-context identity correction has also completed native
validation in `build/native-compute-runtime-identity-final`. Public accelerator
registry IDs and the native allocation-owner service identify different
objects. The corrected buffer uses the driver's exact retained Context object;
the future device factory must still validate the real service hierarchy.
Its focused relink rebuilt only the ObjC buffer object and verified the four
tested C++ objects remained byte-identical. All four artifacts were exported
and the 40,960,697-byte stage removed. Compiler-backed offline tests were not
rerun during this relink; their unchanged C++ objects retain the original proof.

- Identity relink report SHA-256: `32d4289e847998399330a48f164b13baef9ec7d82e45898c71d28dc7a55de3db`.
- Corrected runtime dylib SHA-256: `7b59b0e0c3f05f92377a0350bc40fbba93f1bd1e156edad6769b8ecc8c62721d`.

`post-export-verification.json` independently binds the five frozen source
hashes, four identical reused objects and every exported artifact. Neither
native runtime build instantiates the Metal object or executes the target GPU.

## Separate native metallib compiler evidence

`build/metal-metallib/native-run-2/report.json` records all 17 native commands
passing and 23 exports verified. The linked compiler processes seven actual
Apple-compiled SDL functions from the measured MTLB 2.5 container; each result
matches independently sliced AIR and passes independent SPIR-V validation.
Twenty-five negative C ABI cases reject malformed inputs or name/stage
disagreement. Old AIR/LLVM formats, dispatch planning and two-kernel
local64/local3 ACO pipeline regression checks also pass.

- Compiler dylib SHA-256: `7d12193d9dde38cd97ba16ec241d1c4a6ad35e90b36b604dc964da0901ac7d24`.
- Source archive SHA-256: `dd2ce87ebf52cf38e00249736b383979d7b4e2665f9984426405d330da76dcdc`.
- Native report SHA-256: `ea4a5cbe9a2cffad8f7a7c8f732c8dcf1eb6ed6f6d4228bd60bfada45c698946`.

All artifacts were exported, source hashes matched at completion and the
158,219,947-byte stage was removed. Native CLI execution was explicitly skipped;
separate local CLI checks and 15 Rust library tests passed. The positive
metallib fixture contains vertex/fragment functions. Modern macOS compute
containers, GPU execution and Metal device registration remain unverified.

The later additive owned ABI is native-validated separately in
`build/metal-metallib-owned-native-final`. Its 18 commands pass, including 460
owned-library ABI checks and recompilation of all seven actual SDL AIR
functions after freeing their library/input owners. The earlier 23 compiler
APIs and the 25 negative cases remain covered; exactly 31 public symbols were
verified in the linked Mach-O. Local tests also passed all 19 Rust library
tests and the CLI. Native CLI execution was explicitly skipped.

- Owned-ABI dylib SHA-256: `876680e8255777987cbbe9a1c9b3acf42047a8c7281658bcb2cf7fa95ceb25cc`.
- Owned-ABI report SHA-256: `f56be45bc6ab434b6cf435ef535f029522515ac529cb18fc9c720b43418987e9`.
- Source archive SHA-256: `5c12a99aa822c5122b1969e151632d27b6e7bab3b6799e9c261a3405c017dad8`.
- Post-export verification SHA-256: `edb92cdd8c5f12224d22e58d24a06bd2b7b459bc961acfad7dd7217c0b2f03d2`.

All archived/current compiler sources and exported artifacts matched. The
stage `/Users/victor/Library/Caches/HackyNavi-MetalNative.DVEUu6` was removed
after reaching 158,332,943 observed bytes. This preserves the measured MTLB
2.5 format; it adds owned copying, not a new container layout or Metal device.

The separate target capture is
[`HackyNavi-live-capture-45eab551a719`](../build/releases/HackyNavi-live-capture-45eab551a719/HANDOFF.md).
It contains five selected capture changes from the previously loaded snapshot;
none of this product transport or later engine-consumer work is substituted
into that immutable test bundle.
