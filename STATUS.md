# HackyNavi current status

Latest integration: [combined graphics preparation](docs/validation-2026-10-11-combined-graphics-evidence.md).
Full native core/aux and updated Metal/tool builds pass; early recovery, paired
function and partial display/DMA evidence are now retained. The next target
test remains withheld until inherited-framebuffer ownership is implemented.

Updated 2026-10-11. This is the current work/evidence index; implementation
details live in [architecture](docs/architecture.md), dependencies and the full
objective in [PLAN.md](PLAN.md), and build/continuation instructions in
[HANDOFF.md](HANDOFF.md). Refresh this file after each integrated milestone.

## Current outcome

The `3fa` target boot succeeded: exact binary UUID matches, both services are
present, all 12 collection commands returned success, and live-capture analysis
has no errors. The 32 new context registers are complete and unchanged across
both passes. Pipe0 timing is active; timing1–3 is stopped, with those HUBP
functional clocks disabled. This still does not establish pipe0's TLB0 address
mapping or authorize releasing the dormant requesters. Evidence is retained at
`build/target-evidence/live-3fa3f6d4723b-20261011-163144-730` and interpreted in
`build/target-3fa-display-review-1/report.md`.

The user requires the next requested target boot to test actual graphics, with
all necessary diagnostics collected in the same run. No further capture-only
package is to be issued. The full prerequisite/collection audit is complete;
the current source still has unresolved startup gates, so no graphics test
package is ready yet. Successful rendering is not implied by this release gate.

Since that target test, native typed PSP display-session ownership/retirement
and the portable DP pixel-stream/MSA path have been implemented. Four native
PSP/reset owner units compile against real headers; typed/image/ring fault tests
pass strict and UBSan builds. DP passes 291,842 assertions in each focused build
and 291,840 assertions through CMake, plus generated-register and kernel-header
checks. These checks do not exercise GPU hardware. Native DP modeset integration
remains outstanding.

The combined-preflight native build completed at 20:57:59 UTC on October 11:
`build/macos-combined-graphics-preflight-1/build-report.json` (SHA-256
`183421b507ce3522638f065ac18216126bd28a592d02ed76cc951e1452868d73`).
It compiles/links 113 core units and two accelerator units, with all 413 core
imports resolved from the build Mac's BootKC. Native dependency checks pass;
kmutil still reports the expected developer-stage ownership and SDK baseline
warnings. The build is not installed or target-tested, and is not a release.
Its 11,456 KiB remote stage was removed. Later source changes require a new build.

The updated 22-unit Metal frontend and all four public tools also compile/link:
`build/graphics-admission-frontend-native-1/report.json` (SHA-256
`e2c2e62a1b0bef9f55a287cd2bb92f9352097b7931da1b358cdfb68198255e91`).
This includes bounded actual loader/ABI admission errors. No Metal device was
created or GPU work run on mini; its remote stage was removed.

The combined engine preflight, CP/SDMA inventory and Metal loader failure
reporting are integrated and compiled. Recovery-preparation, paired-function and
partial-read evidence gaps are now implemented. Current work reviews the final
integration and investigates the actual framebuffer mapping owner. The audit is
[next graphics test audit](docs/next-graphics-test-audit.md). A target runner now
serializes actual compute/triangle/fragment/vertex readback tools and saves
before/after plus per-test evidence; it is not an issued test package.
Native OTG activation
also needs the retained bandwidth/calculation binding; no readiness boolean or
fixture result may substitute for it. The new capture package is unchanged.

The accelerator-port route and real vertex-buffer kernel admission, draw,
private capture, v1.7 transport, Context and Metal bindings are implemented.
Actual Darwin compiler, runtime, transport and frontend builds now pass, with
independent source, binary and package reviews. This is build and CPU validation;
engine startup, GPU rendering, Metal discovery and WindowServer acceleration
have not run on the target. Typed PSP HDCP/DTM owned protocols pass focused
validation and CMake integration; native controller invocation is now implemented
but has not executed on the target.

The actual target's display snapshot still rejects after four justified decoder
corrections. TLB0 address semantics and ownership/retirement of the dormant
zero-viewport requesters remain unresolved. The separate read-only
OTG/VTG/MALL/VM-context inventory is implemented, native-built and target-tested. It reads
32 additional registers in two guarded passes and grants no startup authority. See
[address audit](docs/boot-display-address-audit.md) and
[vertex-buffer implementation](docs/metal-vertex-buffer-render.md).

Completed target test: [frozen capture package](build/releases/display-context-20261011/HackyNavi-live-capture-3fa3f6d4723b.zip)
and [OpenCore agent prompt](build/releases/display-context-20261011/OPENCORE-AGENT-PROMPT.md).
Built 2026-10-11 at 16:05 America/New_York, with 112 early-core units and all
412 imports resolved from BootKC. ZIP SHA-256:
`01d9308042ce78a6e98bff88787f97a01242c9390c87f7ab83b650fca5309e8a`.
The separate two-unit late accelerator also builds; it is excluded from this
OpenCore package. The 11,320 KiB native stage was removed. Preserve the tested
`9eda6a40f88c` core as rollback, keep `-hackynavi-device` and `-radvesa`, and
leave `-hackynavi-engines` absent in this historical capture package. Its exact
binary has now booted on the target. Do not ask to repeat this capture.
Independent checks are in `build/vertex-context-native-build-review-1` and
`build/vertex-context-package-review-1`, including the adjacent handoff prompt.

The previous boot succeeded: `live-9eda6a40f88c-20261011-153308-790` has the
expected UUID and both active services. All 12 collection commands returned
success, and live-capture analysis passes without errors. That capture still
fails the scanout address decoder; establishing the actual memory reservation
precedes any engine test. See
[target success and next blocker](docs/validation-2026-10-11-9eda-loader-success.md).

Previous c39 result: OpenCore rejected the binary before load.
The copied USB log explicitly reports IOGraphicsFamily absent from BootKC,
vtable patching failure, and injection `Invalid Parameter`. The core's hard
IOAccelerator dependency caused the failure; successful native linking across
all kernel collections did not establish early injection compatibility.
The early core / late accelerator split is implemented, independently reviewed
and native-linked, with a new BootKC import check in packaging. See
[exact target evidence](docs/validation-2026-10-11-c39-loader-failure.md).
Do not repeat the unchanged c39 test. The earlier c39 package directly under
`build/releases` was never issued: it retains a corrected packaging-metadata
detection failure. Both c39 archives are historical; use the new
`display-context-20261011` package above is the latest completed capture test.

The bounded native offscreen graphics path is integrated. It is not yet target-tested
and no working Metal render device is claimed. The current bounded draw is one
triangle into a linear RGBA8 target, using actual admitted NGG/fragment code.

Current graphics integration evidence:

- `build/macos-vertex-display-context-1`: current 112-unit core and two-unit
  late accelerator, with 283 archived source inputs. Core SHA-256
  `3fa3f6d4723bdd08e28de0f8ce49b6c0f3a7af87013f3a6c0e87e66b9d7fbfc5`,
  UUID `F2DDA1C3-D4B2-3C4E-AF63-E70C25298DAC`. The build includes v1.7 vertex
  admission/capture, portable typed PSP protocols and guarded display-context
  inventory. Actual 412 BootKC imports and frozen sources are independently
  verified. Recorded staging-ownership and SDK deployment warnings remain;
  native linking does not establish target runtime compatibility.
- `build/graphics-vertex-buffer-darwin-native-1`: 270 actual compiler checks,
  16 genuine AIR/metallib compilation products across four profiles through CLI/C ABI routes,
  six exact graphics exports, 22 unsupported vertex fixtures and 249,778
  layout checks over 493 Mesa products pass. The remote stage was removed.
  `build/graphics-vertex-runtime-native-1` passes 96 ASan/UBSan checks across
  eight real compiler-backed pipelines; its stage was also removed.
- `build/vertex-buffer-native-transport-1`: native transport and four tools
  compile; 160,058 sanitizer protocol checks pass. `build/vertex-frontend-native-1`
  compiles all 22 frontend units and four target tools in 48 native commands.
  The public Metal vertex-buffer tool checks two uploaded triangles and
  binding/offset lifetime through actual target readback, but has only been
  compiled, not run. Bundle SHA-256
  `ff472f87886e463f9ac1eef57cac83a91a651913c594c5f8bfbc2520ed79bcab`.
  Both remote stages were removed; no Metal device was opened on mini.
- `build/metal-vertex-development-package-2`: latest userspace development
  archive, 38,745,093 bytes, 137 files, 4,394 source members and 15 Mach-O images.
  SHA-256 `e0d101fe275a7621a0056560df68881bcd0319176ad5ef02ab638c0cde93f79d`.
  Packaging validation rejects 24 corruptions; independent source/package
  review passes 8,215/9,131 checks in `build/vertex-graphics-independent-review-1`.
  This is separate from the boot-capture ZIP and is not installed or target-tested.
- `build/macos-accelerator-split-1`: previous, target-verified 110-unit core plus two-unit genuine
  IOAccelerator bundle, 279 archived inputs, ten real embedded firmware images.
  Core executable SHA-256
  `9eda6a40f88c2a2c42f33dc2e6ef6c2593d070865f64927c83b12a46860b34de`,
  UUID `71CBC771-1C22-370B-B1E9-8D1FB1C0E211`.
  Auxiliary executable SHA-256
  `57d8b8d982c9d65b9cac83fc8bd2e6f059972b5b7494ac1c4d23affa18a003e6`.
  Source/ownership review is `build/accelerator-split-review-1/report.json`;
  packaging regression checks are `build/boot-collection-gate-review-2/report.json`.
  The genuine late service preserves exact frontend identity and uses a
  versioned kernel-only provider protocol; no second hardware owner is created.
  Native dependency lookup passes; staging ownership and SDK deployment
  warnings remain recorded. Nothing was installed or loaded on mini.
- `build/psp-ta-native-local-2`: real HDCP/DTM TA image parsing and retained PSP
  LOAD/UNLOAD ownership, guarded native DMA storage and retirement integration
  pass 892 strict, 892 UBSan and 72,746 ring checks. Four kernel-header objects
  compile with a 760-byte maximum frame. Both CMake TA tests and ten-image
  firmware verification pass. Fresh TA ownership does not establish inherited
  HDCP state; no typed application invocation or startup activation is enabled.
- `build/metal-buffer-native-1`: all 22 frontend units compile and link, including
  actual Context and Metal fragment-buffer submission. All three target-only
  tools compile (45 native commands, six exports); the 62,302,702-byte stage was
  removed. Bundle executable SHA-256:
  `8371b4998c843245f29e45c53cadcb0338566321118327391047f091443274b5`.
  The independent Metal source review is
  `build/metal-render-buffer-local-1/display-capture-independent-review.json`.
  No target tool or Metal object was run.
- `build/metal-buffer-bundle-package-1`: matching development archive built,
  with 130 files, 4,343 source members, 13 native images and exact Apple fixtures.
  Package validation and corrupt-binary rejection pass in
  `build/metal-buffer-bundle-package-check-1`. Archive: 36,569,742 bytes,
  SHA-256 `c46b6d6c6fa3a0474f4002dc3914750ee61f7bbe5beab700876a6d6f1c3e14e8`.
  It is not installed or loaded and does not establish engine-test readiness.
- `build/graphics-buffer-darwin-native-1`: actual Apple linker exports the three
  real graphics compilers and buffer descriptor oracle. All 88 native compiler
  checks, twelve actual AIR/metallib products and 249,778 color-layout checks
  over 493 Mesa products pass. The 215,811,700-byte stage was removed.
  `build/graphics-buffer-runtime-native-1` then passes 54 ASan/UBSan checks over
  four genuine compiler-backed pipelines with six instrumented authored objects;
  its 63,100,054-byte stage was removed. No Context or Metal object was opened.
- `build/macos-fragment-buffer-integration-1`: actual fragment-buffer instruction
  admission, descriptor encoding, retained kernel capture and v1.6 wire protocol
  compile in the full 109-unit kext with all 414 imports resolved. Independent
  verification checks 271 source inputs, six bundle files and nine real firmware
  images. Executable SHA-256:
  `c39afcee43f96070f8a230f11ff890360cc6edbe2bc2bb7ab67ef743b9560180`.
  The 10,444 KiB remote stage was removed. This is an unsigned development build;
  no loading, engine execution or render occurred.
- `build/native-buffer-transport-1`: actual v1.6 IOKit transport and four target
  tools compile; 160,054 ASan/UBSan protocol checks pass. The 1,184 KiB stage was
  removed. The actual Context draw retains the fragment BO, validates its logical
  bounds and context, maps it read-only, waits for native completion and unmaps
  it; its Darwin object compiles in `build/context-buffer-darwin-local-1`.
  Metal buffer binding/command recording is now native-linked as recorded above.
- `build/gfx11-fragment-buffer-admission-local-1`: the actual 72-byte ACO shader
  is admitted independently; descriptors derive from the retained mapping and
  the new draw is 720 DWORDs. Strict/UBSan boundary and mutation tests, real Mesa
  descriptor comparisons and kernel-header compilation pass. Native capture and
  wire reviews pass in `build/native-buffer-capture-local-2` and
  `build/native-buffer-wire-local-1`. See
  [fragment-buffer native path](docs/graphics-fragment-buffer-native.md).
- `build/runtime-graphics-buffer-local-1`: actual AIR/metallib translation and
  immutable pipeline capture pass 7,328 strict and UBSan checks each. Native
  compiler and real POSIX runtime-builder validation now pass above; these offline
  tests do not open a GPU context. The older constant-color profile is preserved.
- `build/boot-hdmi-stream-local-1`: inherited AVMUTE, three actual observed frame
  cycles and AFMT sample-send muting pass focused failure tests, a 271-register
  source oracle, kernel-header compilation and independent review. CMake's new
  HDMI/buffer/ABI tests pass in `build/buffer-hdmi-cmake-integration-1`. No native
  HDMI mutation is enabled: inherited protection and transmitter ownership are
  still missing. Real PSP TA ownership is the next display prerequisite.
- `build/boot-backend-stop-local-1`: separate inherited MPC detach and terminal
  OTG stop transactions pass targeted strict/UBSan failure tests, the 129-register
  source oracle and real kernel-header compilation. CMake build/test integration
  passes in `build/boot-backend-stop-cmake-integration-1`. The implementation
  preserves FIFO enable where the actual DCN32 callback table has no disable
  operation and rejects reasserted timing activity after the final idle poll.
  It is not connected to native mutation; HDMI sink/protection/transmitter
  ownership is still required before the terminal timing step.
- `build/macos-boot-backend-integration-2`: the actual inherited display backend
  reader is integrated after inherited DMCUB stop, under the real FunctionGroup
  operation. All 109 kernel units build and all 414 imports resolve. Changed
  hardware and transport/access failures abort startup; unsupported display
  profiles remain observations and grant no takeover permission. The report is
  published under `InheritedBackend` in engine diagnostics. Root independently
  verified all 269 archived inputs and six bundle files in
  `build/boot-backend-native-parent-2`. Executable SHA-256:
  `793a149327ae87225b20c786df572defb78614a75c38e3a181f1c1f8e1e5b6ad`.
  The 10,396 KiB remote stage was removed; no load or GPU execution occurred.
  Integration-1 predates the precise native failure mapping and is superseded.
- `build/boot-display-backend-local-2` validates the read-only 117-register
  HDMI route classifier against actual DCN3.2 addresses; native access is covered
  by `build/boot-backend-access-local-1` and the newer integration build above.
  CMake registration passed in `build/boot-backend-cmake-integration-2`.
- `build/boot-opp-blank-local-1`: implemented the actual single-OPP blanking
  sequence, using observed OTG dimensions, both RGB color banks, exact seven
  sourced writes, and actual pending/readback checks. Independent review, 428
  strict and 428 UBSan checks, all 52 register addresses, and real kernel-header
  compilation passed. This portable transaction has no native mutating callsite
  yet; complete inherited HDMI ownership and teardown remain required.
- `build/metal-runtime-source-audit-final`: actual framework captures establish
  that normal MSL source compilation needs a device `featureProfile`, including
  the uncached path with explicit language version. The production source API
  stays explicitly unsupported; no fabricated profile or compiler result was
  introduced. Next shader work adds one real fragment-buffer input through the
  existing precompiled metallib path.
- `build/metal-graphics-bundle-package-final-1`: the complete development bundle
  now includes the validated graphics libraries, compiler helpers, both target
  tests and exact Apple metallib fixtures. Root independently verified 124
  package files, 4,331 source members and all 11 native images/dependencies in
  `build/metal-graphics-package-parent`. Archive SHA-256:
  `dd501a13929d4f4145fddc1daa3ffd40be8a6e0afbcbd2f1b78a5a4a7d7c34ec`.
  Size 34,500,328 bytes. It is not installed, loaded or a claim of target readiness.
- `build/metal-graphics-native-2`: all 21 frontend units compile and link into
  a real Metal bundle and dylib; 41 native commands completed without diagnostics.
  Five exports include both compute and render target tests. Bundle SHA-256
  `e74c5be08b545ce292cdaf34742713cc91b41e3d14058ac7ab2a9c33537ef707`.
  The 62,024,605-byte peak stage was removed. No Metal object was instantiated
  and neither target test ran. Independent verification in
  `build/metal-graphics-parent-2` confirms all 63 inputs, 21 archive objects,
  five exports, actual custom dependency resolution and Device/render method
  metadata. The matching package is listed above.
- `build/graphics-darwin-native-4`: actual Apple relinking now produces the
  exact graphics and color-library C exports. All 52 AIR/metallib compiler
  checks and 493 color-layout checks pass on the Mac; real stage machine bytes
  match the retained products. The native stage was removed.
- `build/graphics-runtime-native-1`: 24 ASan/UBSan checks over two genuine
  compiler-backed graphics pipelines passed, including input and shader
  lifetime ownership. Five instrumented objects and the test executable were
  exported; stage removed. Reused compiler libraries were not instrumented.
  No Context, Metal object or GPU was used by this offline test.
- `build/macos-graphics-draw-metal-y-1`: corrected Metal Y viewport compiled in
  the full 107-unit kext; all 413 imports resolve, stage removed. Independent
  archive verification passed in `build/graphics-metal-y-parent`.
  `build/gfx11-graphics-draw-metal-y-2`
  passes 111,240 strict/UBSan checks each, actual Mesa viewport comparisons and
  independent review. These are build/CPU results, not target execution.
- `build/native-graphics-draw-integration-1`: 24 real kernel objects and five
  native ASan/UBSan suites passed; 6,688 KiB stage removed. The two host warning
  nits were subsequently corrected and compiled with warnings as errors.
- `build/macos-graphics-draw-integration-1`: 107 translation units, all 413 kernel
  imports resolved, 260 archived inputs and six bundle files independently
  verified. Executable `1799ed59a834eb979ed60e616a4c06bceadceb760f44cf5ae5455711156adf48`.
  Its 10,288 KiB stage was removed. No load or GPU execution occurred. This
  positive-Y build predates the Metal viewport correction and is not a test release.
- Real graphics runtime: immutable admitted shader bytes, exact target owner,
  submission, real fence wait, result history release and cleanup;
  `build/runtime-graphics-local-1`, 2,012 strict/UBSan checks each.
- Actual stage-aware shader/library work: `build/runtime-graphics-shader-local-1`,
  5,248 strict/UBSan checks each, including genuine Apple AIR/metallib products.
- Real texture storage/row transfers: `build/metal-texture-local-2`, 170,603
  strict/UBSan checks each. Native Objective-C integration now compiles/links.
- Root wrote the render pipeline and serial draw/texture-sync command path.
  Independent review fixed runtime initializer bypasses and expected compiler
  diagnostic-file cleanup. Native compilation additionally found missing
  include-file nullability defaults and an inherited SDK26 barrier selector.
  Both were fixed without disabling warnings; all 21 objects now compile
  cleanly (`build/frontend-compile-only-native-4`) and full linking passes.
  The target render smoke compiled; it must never execute on the build mini.
- Actual Darwin compiler validation caught unexpected exported C++ runtime
  symbols before running fixtures. Evidence `build/graphics-darwin-native-1`
  preserves the failure. Native relinking then caught mismatched libc++ ABI
  (`native-2`); matching real runtime archives fixed the graphics library link.
  `native-3` caught duplicate `__umoddi3` supplied by both real addrlib and
  compiler_rt in the color library. The worker is correcting that library's
  actual link inputs in native-4, which passes. All three failed terminal stages
  were removed; no fixtures ran in those failed attempts.
- The genuine Apple vertex output does not invert Y; the prior positive-height
  hardware viewport would mirror it. The corrected negative-Y viewport now has
  its own verified oracle and replacement native build, described above.

Reviewed kernel foundations:

- Graphics VA placement: 129,419 strict and UBSan checks per run; private code,
  IB and attribute ring stay in one 4 GiB window. Evidence:
  `build/gfx11-graphics-memory-local/parent-review.json`.
- Actual harvested topology: strict GC1.2 ROM metadata plus serialized hardware
  SA/RB/WGP observations; 2,023,453 strict and UBSan checks per run and four
  actual kernel-header objects. Independent review:
  `build/gfx11-graphics-topology-local/vcn-independent-review.json`.
- Corrected canonical GFX11 draw: 712 DWORDs; 78,407 strict and UBSan checks per
  run, 15 actual shader-product streams compared with real Mesa, and every
  emitted register address checked. Independent review caught eight GFX10-only
  color writes in the prior local-2 version; local-4 fixes them and preserves a
  regression check. Evidence: `build/gfx11-graphics-draw-local-4/`.
- Prepared-but-never-started queue retirement and separate no-queue RLC
  retirement now have real ownership/physical checks. The latter never invents
  CSIB ownership, requires observed zero binding and disabled SRM, and gates PSP
  destruction on actual RLC retirement. 38,574 strict/UBSan checks per run and
  seven actual kernel-header objects pass; root reviewed
  `build/noqueue-rlc-retirement-local-1/parent-review.json`.
- v1.5 graphics wire envelope preserves all 25 legacy structures and old
  selector numbers. Strict/UBSan: 159,950 checks each; independent review at
  `build/graphics-wire-local-2/display-independent-review.json`. Real Mac
  transport compile/link and ASan/UBSan passed in
  `build/native-graphics-transport-1`; all 14 inputs/five outputs verified and
  the 1,128 KiB stage removed. No IOKit connection was opened.

Atomic native graphics capture now owns private DMA code/IB/attribute storage,
validates the complete mapping ledger and distinct CPU/DMA ranges, binds real
private topology and uses existing GPU-fence/RPTR completion and consumption.
Its final local snapshot is `build/native-graphics-capture-local-3`; independent
ownership review passed. ClientSession/UserClient and transport are connected,
and explicit v1.5 negotiation is enabled. The runtime and public Metal objects
are written; their combined native validation and target execution remain.
The six focused CMake suites pass in `build/graphics-wire-cmake-1`.


The full Metal frontend now compiles and links in `build/metal-device-native-2`:
14 actual translation units, 31 successful commands without diagnostics,
40 independently verified input members and four exported artifacts. This
includes a real Mach-O bundle, `HackyNaviMetal`, SHA-256
`837ee6bc0e57cabc235b104fae37965d3e411828de40030e6cdb5055ef18cacc`.
It uses the genuine `_MTLDevice` superclass, exact framework-contract admission,
actual native Context, owned limits and `MTLArchitecture`, and explicit helper
paths. Root verified every static archive object; the 43,559,163-byte observed
stage was removed. The first failed availability-annotation build is preserved.
Static inspection now matches all 391 implemented required method encodings;
the sole remaining required notification method is inherited from the genuine
framework superclass. Two independent reviews passed the bounded Mach-O checker.
The complete development package is `build/metal-bundle-package-final-3`, archive
SHA-256 `c78cceec88dfb01d1337a8576fab3d573fbae036482cb6af39e3704599aecd43`.
Root independently verified all 88 package files, 4,198 source members and the
five unchanged native binaries. Explicit native-macOS platform, deployment,
dependency and archive-mode checks pass. No installation, device construction,
target discovery or GPU execution has occurred.

Coordinated display-clock controls now pass independent review and native Mac
validation: 306,452 ASan/UBSan checks and eight actual kernel objects in
`build/native-display-clock-integration-final`. Root verified all 88 archived
inputs; the 2,120 KiB stage was removed. This includes one-shot UCLK maximum,
DCF sleep floor, sourced divider planning, stable physical PLL/DFS capture,
and the same-owner Display/Firmware acquisition and retirement bridge. The
complete kext below includes them. No hardware clock operation has run.

Engine retirement now keeps DMCUB running until queue/VM/RLC and coordinated
clock retirement finish. Display consumer and firmware phases report their real
results separately. Independent review and native validation passed 71,556
ASan/UBSan checks and nine kernel object compiles in
`build/native-engine-display-retirement-final`; all 105 archived inputs were
verified and the 2,956 KiB stage removed. The graphics barrier matches actual
pinned Mesa output. These protocols have not run on the target.

The offline graphics compiler now translates actual Apple-generated vertex and
fragment AIR and metallib through the real NGG/fragment adapters. The captured
Apple fixture and all four resulting products passed independent review;
`build/apple-graphics-integration-1` records 36 checks. AIR and metallib produce
identical stage code. This was compiler execution only, with no GPU access.

Real Mesa/addrlib RGBA8 target layout is implemented and independently reviewed
in `build/gfx11-color-target-local-6`: 493 actual Mesa products per run, with
strict and authored-adapter UBSan checks passing. The numerical layout is
address-free; it does not grant resource ownership. The kernel graphics shader
admission implementation in `build/gfx11-graphics-product-local-2` passes 97,170
strict and UBSan checks each, including 40 pairs of actual compiler products.
It is registered in CMake, host tests and kext builds; CMake validation passed
in `build/gfx11-graphics-product-cmake-1`. Independent ISA review passed (`vcn-independent-review.json`).

The native cold DISP/DPP clock owner is implemented and independently reviewed:
actual firmware/DPM authority, private retained journals, observed PLL/DID
verification and the sourced DID126 prelude when entering DID127. DTO and
blanked surface preparation now require fresh checks against those owned clock
references. Local real kernel-header compilation passed in
`build/native-global-clock-local-2`; the portable transaction passes 21,297
strict/UBSan checks each in `build/cold-preset-local-2`. The combined native Mac integration run passed as described below. Startup remains disconnected until mode selection and recovery
are complete.

Scanout power guards now accept the stable current-ON encoding from the target
logs and retain the complete status/configuration pair across access. Root
review passed in `build/scanout-power-local-2`. The real native color-topology
capture also passed root review; it binds matching PCI/GC/NBIO observations to
actual GART/Firmware/Operation ownership and has no automatic startup caller.
Root moved the existing ~2 KiB GART plan workspace from kernel stack into the
mutex-owned controller; both local and native 2 KiB frame checks now pass.

Combined native validation is `build/native-graphics-clock-integration-1`:
eight ASan/UBSan suites passed 41,992,134 assertions; 19 kernel sources compiled
without diagnostics. Root verified all 119 archived inputs and object hashes;
the observed 6,444 KiB stage was removed. The complete 102-unit kext is
`build/macos-graphics-clock-integration-1`, executable SHA-256
`0624ac849a0a33d8654d9fab813d4a416ef25ea07f850033d01fc31534faaac1`, UUID
`37C226DC-23DD-381D-81EF-B605EB266468`. Root verified 252 source files, all six
bundle files and all 412 imports. Its 10,116 KiB stage was removed. Existing
staging-ownership diagnostic errors and SDK libkmod deployment warnings remain
recorded; this is not a loading or GPU test.

Workers now implement atomic native graphics capture/VA placement/live harvested
topology, complete sourced GFX11 draw commands, and never-started prepared-queue
retirement. Root owns integration, independent review and the native build lane.
No rendered output or macOS Metal execution is established. The already-issued
live-capture diagnostic remains the supported target test; this newer development
binary does not replace it.

The AIR helper now uses fixed system GPUCompiler/MTLCompiler library paths.
Its environment redirect was removed. A focused native run passed real AIR,
invalid AIR and bogus-override tests plus 94 explicit compiler ABI checks;
the previously verified compiler was reused byte for byte. Root verified all
46 inputs and both exports in `build/metal-system-reader-native-final`, and
the stage was removed. This path requires the system reader API, not Xcode.

The explicit compiler-tool integration now passed native validation in
`build/metal-explicit-tools-native-final-2`: all 22 commands, exactly 32 exports,
94 explicit C ABI checks and 27 actual C++ ASan/UBSan ownership checks. Root
verified all 44 archived inputs and both exports; the stage was removed.
The compiler dylib SHA-256 is
`1be08ab74e61e05ebfe49caf739f8986e118740f8e0ece4938534b2cb6d85e2d`.
Device/Library now forward copied absolute helper paths through AIR translation
and subsequent pipeline validation without changing process-global tool paths.
The earlier failed harness launch remains recorded: LeakSanitizer was unavailable
on macOS; the successful run retained AddressSanitizer/UndefinedBehaviorSanitizer.
No GPU execution or Metal discovery is established by these compiler tests.

DF C-state policy now has an explicit driver-owned DISALLOW acquisition and
terminal ALLOW release, with retained failure history and native cold-state
checks. It does not claim restoration of an unknowable previous allowance.
Independent review passed; strict local/UBSan and native ASan/UBSan suites each
pass 153,589 checks. Four kernel objects compiled with a 2 KiB stack-frame limit
in `build/native-smu-df-cstate-final`; root verified 88 archived inputs and the
2,616 KiB stage was removed. The complete display-clock transaction remains
separate work. See
[DF policy](docs/smu-df-cstate-policy.md).

Active integration now includes actual Metal Managed buffers: an owned CPU
copy, real native uploads from `didModifyRange`, and ordered blit readback from
the actual GPU allocation. Buffer ownership and mixed compute/readback command
ordering passed independent source review. The frozen eleven-object native
compile/link passed all 22 commands without diagnostics, and the public-Metal
target smoke executable compiled. Root verified 32 archived inputs, all eleven
static archive objects and three exports in `build/metal-managed-native-1`;
the temporary stage was removed. No Metal object or GPU execution is claimed.
This snapshot excludes Device/Capabilities and the newer explicit compiler API.
The complete device frontend superseding it is described above. Runner review found
two evidence-preservation/accounting improvements; future runs now reject any
nonempty output directory and measure storage after archive creation too.
The successful build and its original reports remain unchanged.

SOC/Fabric deep-sleep policy now has source-reviewed exact captured-bit
inhibition/restoration, with private failure retention. The focused Mac run
passed 138,600 ASan/UBSan protocol checks and compiled four kernel objects;
88 archived inputs were independently verified and the 2,548 KiB stage removed.
This focused check preceded the latest full kext below. The later DF C-state work is
described above; the complete display clock transaction remains separate. See
[policy implementation](docs/smu-soc-fabric-deep-sleep.md).

Prior complete kext (superseded by the 102-unit build above): `build/macos-engine-display-retirement-final` compiles 98
translation units and resolves all 412 imports. Root verified 245 source files
and six bundle files. It includes the ordered Display/Engine retirement,
clock controls and graphics barrier. Executable SHA-256 is
`e242c1b221a133bcad72f8f199572bb48e18c59c924feef7d4ac5e05ef334c42`;
UUID `7C04C291-FB69-3E7B-A8CE-68285CE168B4`. The 9,976 KiB stage was removed.
The no-load diagnostic retains staging ownership errors and SDK deployment
warnings; dependency resolution does not establish target loading.

Previous complete kext: `build/macos-accelerator-final` compiles 94 translation
units and resolves all 412 imports, including the public IOAccelerator base.
Parent validation checked 239 source files and six bundle files. Executable
SHA-256 is `12633219109b34fd9440e07bb2662cf462595bc742e18f9ed2aa6ae908bd4065`;
the 9,868 KiB stage was removed. This integrates the reviewed accelerator child
and FAMS/DAL operation, with service creation bound to verified engine startup
and service termination bound to engine retirement. See
[service ownership and evidence](docs/metal-accelerator-service.md).
No target load, device discovery or Metal GPU execution is established.

Display firmware coordination is now implemented and natively checked:
the DAL MCLK-wait policy is disabled and acknowledged before the actual
DMCUB FAMS-disable command is submitted and consumed. Scope/deadline checks
cover the shared-memory and flush callbacks, and partial changes retain
their owner until verified reset. The focused Mac run passed 29,370
ASan/UBSan checks and compiled three real kernel objects with a 2 KiB stack
frame limit; the 1,992 KiB stage was removed. This operation still needs a
complete clock-transaction caller and target execution. See
[clock coordination](docs/display-clock-hold-design.md). The latest full build
includes this operation. A subsequent bridge allowing UCLK requests with the
same owner's completed FAMS handoff passed independent review and native
kernel compilation; its 604 KiB stage was removed. It is now included in the
latest full build above and has not run on hardware.

Latest integration: native ISA admission now accepts the actual one-thread
uniform compute products and packed local-X operation required by the
multidimensional products. Independent review and 220,693 focused macOS
ASan/UBSan checks passed; the changed kernel object compiled. The stage was
removed. Nine real compiler cases/twenty variants also pass 265 native
ASan/UBSan checks, including a freshly Apple-compiled macOS15 AIR and compute
metallib. The Metal compute pipeline wrapper now compiles and links natively;
seven previously validated objects were reused byte-for-byte. The updated
MTLB2.8 compiler passed 460 existing and 48 new native ABI checks with exactly
31 exports. Both stages were removed and all inputs/artifacts independently
verified in [the parent check](build/compute-components-parent-verification.json).
See [the implementation](docs/gfx11-uniform-compute-admission.md).
The admission changes are now in `build/macos-compute-admission-final`:
93 translation units,407 resolved imports,237 verified source files and six
verified bundle files. Executable SHA-256 is
`cdd9f0fa87442493c9e7b0286c0b0a718e884c43bcdcbf81d68ffd7977b82207`.
Its9,836KiB stage was removed. The build also compiles the reviewed CPU-only
task-memory owner, which has no client-protocol callsite yet. The no-load
diagnostic retains staging ownership501:20 versus0:0 errors; it does not
establish loading. This build has not run on the target.

Metal queue, command-buffer and compute-encoder objects now compile and link
natively with the actual runtime. The focused run passed 55,881 ASan/UBSan
host lifecycle checks and 17 native commands; all nine previous objects were
reused unchanged. Root independently verified 37 archived inputs, six exported
artifacts and those nine object byte sequences. The 42,435,464-byte staging
directory was removed. See [parent verification](build/compute-command-parent-verification.json).
This validates compilation and host scheduling, not Metal object execution.

The actual device frontend, bundle and public IOAccelerator service are now
implemented and native-linked as described above. The measured loader matches
that class and accepts subclasses of the actual `_MTLDevice` base. Base
initialization and capability implementations remain untested at runtime;
discovery is not yet demonstrated.
The synchronous native submission already waits for actual GPU completion;
command status must reflect that observed lifecycle rather than invent an
earlier completion or GPU timing.

The preceding userspace milestone is native-compiled `HNMetalLibrary` and compute
`HNMetalFunction`, backed by owned metallib extraction and a retained actual
AIR shader. The new Shader layer passed 252 macOS ASan/UBSan checks across
five cases and fourteen ACO variants. The compiler's owned-library ABI passed
460 native checks and seven post-free AIR compiles, with exactly31 exports.
All exported artifacts were verified and both Mac stages removed. See
[Metal integration](docs/metal-native-integration.md). No Metal object has yet
been constructed through discovery, and no HackyNavi GPU dispatch has run on
the macOS target.

Kernel ISA admission now checks each variant's register allocation before
accepting its workgroup or publishing launch registers. Focused native macOS
validation passed 471,170 ASan/UBSan checks across occupancy, compute admission
and dispatch admission; the changed kernel object compiled with real headers.
The 1,020 KiB stage was removed. This is included in the full
`macos-compute-admission-final` build, but not the previously issued diagnostic
package. See
[occupancy evidence](build/native-gfx11-occupancy-admission/report.json).

The `dbbcb5ec0ba9` capture has now loaded on the target with its matching UUID.
All twelve collector commands passed; all fourteen submitted files were
preserved and hashed. All four display domains returned stable STATUS
`0x10000000` and CONFIG0. The prior FORCEON explanation is disproved; the
classifier's CONFIG1 requirement was too strict. VCN0 still returned all ones.
See [the new target evidence](docs/validation-2026-10-11-config0-capture.md).

The corrected current-FSM capture and bounded VCN POWER_STATUS prerequisite
inventory passed native macOS sanitizer checks: 6,592,514 capture, 37,457,089
retirement and 63,258 auxiliary assertions, zero failures. Three kernel objects
compiled; the 3,196 KiB mini stage was removed and source hashes matched.
The selected 74-unit follow-up is now frozen with exactly five selected source
changes: [`HackyNavi-live-capture-45eab551a719.zip`](build/releases/HackyNavi-live-capture-45eab551a719.zip),
with [handoff](build/releases/HackyNavi-live-capture-45eab551a719/HANDOFF.md).
All 353 imports resolve, all 167 source files and 15 packaged files were
verified, and the 7,824 KiB full-build stage was removed. This exact package
has not been booted; engine activation and Metal remain outside its scope.

The v1.3 product transport and real Session/IOUserClient bridge are implemented:
owned generation-bearing staging buffers are copied under the connection lock,
then native ISA admission, private DMA capture, actual queue completion and
retirement determine success. Protocol checks passed 155,702 cases; kernel
objects and independent stack/ownership review passed. A target-only tool now
compiles actual AIR/IR into owned full/tail variants and calls the native path.
Native IOKit tool validation passed: the real transport dylib and four tools
linked, 155,702 protocol sanitizer checks passed, all five artifacts were
exported and the 944 KiB mini stage was removed. The combined kext build and
later boot-display OFF-state focused check are complete, as recorded below.
This work does not establish GPU execution or a Metal device. The six native
kernel objects and three power suites have now passed on mini; its 2,544 KiB
stage was removed. Fresh CMake passed all four selected suites. See
[current integration evidence](docs/validation-product-wire-power.md).

Metal library extraction and compiler integration are now native-validated
in `build/metal-metallib/native-run-2`: all 17 commands passed, seven actual
Apple-compiled SDL functions matched independently sliced AIR and passed
SPIR-V validation, and 25 malformed/name/stage cases were rejected. The
compiler dylib and 23 exports were verified; the 158,219,947-byte stage was
removed. Native CLI execution was explicitly skipped; the separate local
CLI checks and all 15 Rust library tests passed. This supports the measured
MTLB 2.5 vertex/fragment fixture, not unverified modern compute containers or
a Metal device. See [compiler evidence](userspace/metal/METALLIB.md).

Latest frozen native development build: `build/macos-reusable-v14-final`
includes the reviewed v1.4 Session/UserClient, eight historical result slots
and native resource reuse. All 92 translation units built, 406 imports resolved,
234 archived sources and six exported bundle files were verified. Executable
SHA-256 is `67fc0db187d72a5fd459194924d0f17b91dd6f20c3a08c69eee6d19863ea8cb6`;
its 9,808 KiB build stage was removed. Focused native compilation also passed
three actual kernel objects with a 2 KiB frame limit and removed its 616 KiB
stage. This kext has not been loaded or run on the target.

The preceding `build/macos-reuse-final` milestone adds checked
detached-VM reactivation, completed-job consumption that preserves actual data
BOs, and a two-job GPU probe invoked after shader verification under the engine
opt-in. All 92 translation units built, 406 imports resolved and 234 source
files were verified. Executable SHA-256 is
`fe01c5be0f8838dd238fd079cf7709324a81a69d13254e9590f5c50ec854f66b`.
Native ASan/UBSan passed 241,988 VM-context and 5,202 queue checks; eight
kernel objects compiled with a 2 KiB frame limit. The 3,820 KiB focused and
9,792 KiB full-build stages were removed. The probe has not run on the GPU.
The subsequent v1.4 Session/client profile and eight retained-result slots
are implemented outside this immutable build. Independent native ownership
and transport reviews found no defect; strict and UBSan protocol tests each
passed 158,417 checks, and three real kernel-header objects compiled. Its
supplied-source `--reuse` smoke passes object compilation and review against
the actual client. Native transport validation then linked the IOKit dylib and
all four tools, passed 158,417 ASan/UBSan checks and removed the 1,040 KiB
stage. The v1.4 full-kext snapshot above is complete; target execution remains. See
[validation](docs/validation-product-wire-power.md) and
[the resource contract](docs/compute-reusable-resources-design.md).

The later production `compute::Context`/`Buffer`/`Pipeline` layer connects
persistent C++ resource objects to the actual reusable client. Its new
target-only [Context test](userspace/runtime/CONTEXT-SMOKE.md) compiles actual
supplied kernels and checks A->B->retained B->C, guards, stable addresses and
explicit cleanup. Native runtime validation passed 233 actual compiler-backed
ASan/UBSan checks, strict ARC compilation of the private `HNMetalBuffer`, and
linking the real runtime and target tool. The subsequent exact-context identity
correction also passed its focused ObjC relink, reusing four byte-identical
tested C++ objects; all artifacts were verified and both stages removed.
No wrapper GPU run or Metal registration is established, and these userspace
additions are outside the kext archive.

An additive owned metallib C ABI now copies actual function metadata, names
and AIR independently of input/library lifetime. Local validation passed 19
Rust tests, 460 actual DLL ABI checks and recompilation of all seven SDL
functions after freeing the library. Legacy compiler and CLI checks pass.
Independent review found no defect. Native validation then passed all 18
commands and the same 460 owned-library checks, verified exactly 31 public
exports and removed its 158,332,943-byte stage. This source does not expand
the measured MTLB 2.5 format or register a Metal device.

Preceding frozen native development build: `build/macos-product-wire-power-final`
includes the v1.3 Session/IOUserClient bridge, current-state power consumers
and the later boot-display OFF-state correction: 91 translation units,
406 resolved imports, zero unresolved symbols and 233 verified source files.
Executable SHA-256 is
`c1cb8e7f402bc77c43682011e074ba2388f5f99cd673cc22f9f53104d1456aec`;
the 9,724 KiB mini stage was removed. The focused native OFF-state suites
passed 11,554,469 capture and 50,298,906 retirement checks, zero failures;
their 2,532 KiB stage was removed. This immutable artifact predates the
subsequent VM reactivation/reusable-resource work. It has not been loaded
or run on the target GPU. See [validation](docs/validation-product-wire-power.md).

Preceding native development build: `build/macos-product-admission-final` adds
the validated GFX1100 product decoder, multi-region builder and native immutable
capture: 91 translation units, all 406 imports resolved, 233 source files
verified. Native sanitizer suites passed 84,007 decoder, 120,732 builder and
22,512,972 display-retirement checks; both mini stages were removed. The
general product did not include the subsequently implemented Session/IOUserClient
boundary. Target execution remains pending. See [validation](docs/validation-product-admission.md).

Preceding native development build: `build/macos-runtime-recovery-final`, with
88 translation units, all 406 imports resolved and all 228 source hashes
verified. It adds asynchronous retirement for real native-client workload
failures, one-shot pre-MODE1 SMU table reconciliation, and Linux-derived
shader-memory setup for client VMIDs 1..7. Native sanitizer checks passed
127,358 SMU and 17,403 initialization cases; fresh CMake passed all three
selected suites. Both Mac stages were removed. This is not target execution
or Metal readiness. See [validation](docs/validation-runtime-recovery.md).

The preceding PPSMC clock/metrics owner is native-linked in
`build/macos-smu-clock-owner-final`: 88 translation units, 406 resolved imports,
all 228 source hashes verified, and a removed 9,524 KiB mini stage. Real VRAM
metrics ownership and independent table/range retirement are connected to
Engine teardown. 61,081 sanitizer checks passed locally and on the Mac; fresh
CMake integration passed. Request/capture activation still needs the complete
display/recovery contract. See [validation](docs/validation-smu-clock-owner.md).

The preceding compiler/UCLK integration is built in
`build/macos-compiler-uclk-final`: 86 translation units, 406 resolved imports,
all 225 recorded source hashes verified, and six targeted suites passed.
The 9,452 KiB mini stage was removed. The exact compiler shader class now has
native ownership, explicit v1.2 transport, real completion/readback/retirement
and a compiled target-only smoke tool. The scoped UCLK owner preserves actual
firmware fulfillment and uncertain state. Neither path has run on the GPU.
See [the build record](docs/validation-compiler-uclk.md).

The real translator dispatch planner and owned ACO pipeline product now pass
native macOS validation: two actual kernels, each with local64/local3 variants,
23 exports, all16 native commands, and seven Rust unit tests. Review's deadline
and output-budget bug is fixed. A real first variant followed by deliberate
second-variant failure publishes no partial product. The 135,016,925-byte
mini stage was removed. See [pipeline contract and evidence](userspace/metal/PIPELINE.md).
No variant ran on the GPU through HackyNavi. Those runtime-recovery integrations are now linked in the latest build above.
The source-derived GFX1100 leaf decoder, multi-region dispatch builder and
native immutable product capture are now implemented and independently reviewed.
Local tests pass 84,007 validator checks, 120,732 builder checks and 822,327
native interval-helper checks; real kernel objects compile. Build-list
integration, focused native validation and the 91-unit combined link passed.
The later Session/IOUserClient exposure is now implemented as described above;
no general product has demonstrated GPU execution. The tested capture ZIP is
the separate immutable `dbbcb5ec0ba9` build above.

The preceding integration added a real task-bound IOUserClient and isolated
clear/copy sessions, normal completed-VM retirement, and captured board/clock
inputs for the real DML calculator. The 85-unit combined native link resolves
406 imports; all 187 recorded source hashes matched, and 6/6 targeted suites
passed. Aggregate retained results are bounded to 64 MiB / 64 holders. The
first 8,244 KiB temporary Mac stage was removed. The reviewed startup preflight
and raw failure reporting patch is now in the final combined build,
`build/macos-client-preflight-final`: 85 units, 406 resolved imports, all 187
source hashes verified, and its 8,276 KiB stage removed. Executable SHA-256 is
`4f7b7ec5166190d83eb526625b99d363491a6ca717e2c908d9ee540cd82a5ffd`.
The preflight patch additionally passes 7,572,558 display-retirement and 85
console sanitizer checks. This work does not establish GPU execution or a Metal device. See
[the integration record](docs/validation-client-display-inputs.md).

The requested outcome remains a real RX 7900 XTX driver for x86_64 macOS
15.8.1 with GPU initialization, submission, rendering, Metal, HDMI and DP.
It is not achieved. There is no demonstrated HackyNavi GPU execution, visible output,
framebuffer/IOSurface integration or Metal device. The delivered opt-in PCI
package remains diagnostic. Development source now has a separate engine
opt-in which invokes the real startup coordinator, with paired-function
ownership, per-I/O scope checks and retirement before PM ACK/unmap/close.
Its compilation and integration review do not establish target execution.

The user reports macOS is now installed and OpenCore is on USB. Hardware
testing can proceed in stages. Installation remains owned by the other
session. The user explicitly
requests a first diagnostic package and an OpenCore-agent handoff. This is
prepared at `build/releases/HackyNavi-diagnostic-dd4f86a48836.zip`; see
[the handoff](docs/OPENCORE-DIAGNOSTIC-HANDOFF.md). It uses the frozen native
binary in passive observer mode, with no `-hackynavi-device` argument.
The supplied `E:/HackyNaviLogs/20261010-225812-709` logs now prove that this
kext loaded on 15.8.1 / 24H32 and observed the expected physical board. Its
loaded Mach-O UUID matches the frozen executable; BAR dwords/addresses agree.
See [first boot evidence](docs/validation-2026-10-10-passive-boot.md).

The next distinct package is now frozen at
`build/releases/HackyNavi-live-capture-6a3a8173c6e2.zip`.
Its [handoff](build/releases/HackyNavi-live-capture-6a3a8173c6e2/HANDOFF.md)
specifies the `-hackynavi-device` opt-in for BAR5/discovery/ROM/state capture,
including limited selector writes. It does not activate engines or Metal.
This frozen 66-source build has now loaded on the target with a matching UUID.
All twelve collector commands succeeded; discovery, runtime ROM and memory
capture validated, with matching generation 3. The user reports HDMI and no
sleep/reboot/hang/panic before collection. Display classification stopped at
DOMAIN0_PG_STATUS and VCN0 PGFSM returned all ones; neither failure is hidden
by the outer capture's completion. See [live boot evidence](docs/validation-2026-10-11-live-capture.md).
No OpenCore media was changed by this driver-development session.

A follow-up capture package is frozen at
`build/releases/HackyNavi-live-capture-1e4040229efa.zip`, with
[exact boot/collection instructions](build/releases/HackyNavi-live-capture-1e4040229efa/HANDOFF.md).
It preserves the missing raw display power status and per-probe validity with
the same register access sequence. Its 74-source native build links all 353
imports; the 7,788 KiB remote stage was removed. Executable SHA-256 is
`1e4040229efacc1c5e060cfdc5ef4b9ee453c7e7339154bb39f1fe473c0a097a`,
UUID `8DE5BBB4-D07A-3965-B970-3CCE28B7ECAA`. ZIP SHA-256 is
`1cf20912503571e173c8b9a7044ba13afa3340a49641c4779dd1cbb5a2734078`.
It is now target-loaded with a matching UUID; it does not activate engines,
modesets, or Metal. See the follow-up evidence above.
Later VM-registry retirement, audio quiescence and early-media work are outside this immutable
artifact; source snapshots distinguish their validation.

Priority is bootable real Metal; **HDR is explicitly deferred**.

The preceding 81-unit development snapshot is `build/macos-device-engines-final`:
81 translation units (including generated firmware), all 364 kernel imports
resolved, and every recorded source hash matched the working tree at that build. The
3,810,207-byte source snapshot includes actual Device activation, mandatory
GART/firmware/queue/VM/display/Reset/ATOM access scopes, primary retirement,
OwnerFailure cleanup and guarded DAL DPM inventory. The 8,108 KiB remote stage
was removed. No kext was loaded on mini or the target.

Device now creates the real paired-function owner and Lifecycle, registers
retirement before recovery preparation, and invokes Engine on the sleepable
worker only with the separate `-hackynavi-engines` opt-in. Primary PM-off,
termination, shutdown and close join retirement outside the Device mutex
before unmapping or acknowledging teardown. Startup is one-shot. Review found
and fixed a missed primary power-on wake before group creation. Actual callback
ordering, GPU fence/shader output and recovery remain untested on hardware.

The new DAL query reads seven firmware-reported clock tables twice under the
actual operation scope and monotonic deadline. Private provenance retains the
device generation and capture serial. It does not establish measured clocks,
a board DML model, watermark policy or visible scanout. Its new/existing
protocol suites pass 495,670 sanitizer checks; fresh CMake integration passes
2/2. CMake configures 62 suites, not a new full-suite pass.

Executable SHA-256: `c3bcacee8f9285e73f592908437e1d0dbcdef299cf32626d1268090f3b0d4b0a`;
UUID: `70DE9C99-D805-3649-AE60-263FAE5C87C1`. Source archive SHA-256:
`1cef855e309eb4eff6f24824dd4849b78e6c38ca4f2003c0d1fd4ddac6b55bae`.
Build-folder ownership diagnostics and SDK libkmod deployment warnings remain
recorded; successful linking/import resolution is not target-load evidence.
The first `macos-device-engines` build predates the reviewed missed-wake fix;
use the `-final` report for current development evidence. Earlier build
snapshots remain preserved. See [integration validation](docs/validation-device-engines.md).

Historical package `1e4040229efa` captured DOMAIN0 without enabling the engine
path; its 14 manifest files were verified against directory and ZIP. It has
been superseded by subsequent completed captures. Do not request it again.

The display target is now explicit: Samsung Odyssey G90XF, **3840x2160 at
165 Hz**. The user reports both HDMI and DP work in Windows; HDMI is currently
connected. Read-only `QueryDisplayConfig` reports 165/1 Hz, a 1,629,250,000 Hz
pixel clock, 4224x2338 totals and 10-bit YCbCr444. Raw EDID and capture evidence
are retained under `build/monitor-evidence/20261010T234746595065Z`. This does
not report negotiated FRL/DSC. The current TMDS/legacy-DP DML adapter cannot
satisfy that full-refresh timing; FRL, DP DSC and the associated clock/pipe
planning remain required. The existing 4K60 fixtures are not this monitor's
full-refresh validation.

Development stays in `C:/Users/Ras/Documents/ChatGPT/HackyNavi` on Windows.
`victor@mini` is the native build host, with no routine kext loading. Migration
was deferred. Branch: `codex/navi31-bringup`. During this work an external Git
operation amended `b172aac` to `47fa6b9` and origin matched that commit. This
agent team did not amend or push it. Further changes remain uncommitted;
inspect status rather than assuming GitHub contains the final working tree.

## Current implementation and parallel ownership

| Work | Actual state and next handoff |
| --- | --- |
| Native display clocks â€” main agent | Working-tree implementation owns DAL SMU minima and four DPP DTO controllers, saves the acknowledged DPP reference frequency, and serializes access under display/firmware/GART locks. Complete modeset and power policy remain. |
| All-pipe clock admission â€” main agent | `display_idle.hpp/.cpp` checks all four OTGs, HUBPs and MPC routes/idle/locks; brackets power state, avoids gated HUBP reads, rejects address aliases and changed power generations. Read-only admission; it neither stops hardware nor retires memory. |
| Clock teardown | Current code sends acknowledged zero-MHz requests after consumers/display firmware stop. The new source audit did not establish zero as DAL withdrawal; resolve that provenance gap before consuming the DML profile. Actual joint SOC/Fabric/Memory holds and metrics ownership remain required. See [clock ownership design](docs/display-clock-hold-design.md). |
| PCI lifecycle — root / capture review | Real GPU/audio open/PM ownership has a noncopyable thread-bound operation scope, common IOLock and atomic pre-wait revocation. Reviewed native audio binding stops HDA through the owned function's assigned BAR0, clears BME and observes nonposted Transactions Pending; native link passes. No whole-GPU retirement is inferred. Device now instantiates/registers the group under the separate engine opt-in; descendant guards and primary retirement are native-linked. Target activation is untested. |
| Audio protocol | Real GCAP stream enumeration, stream/command-ring/position-DMA stop, reset readback and measured reset hold implemented. 1,066,721 ASan/UBSan checks and real kernel-header compilation pass. Included in all build paths; no target execution. |
| Cold media startup — capture_aux | Early SMU version/wake protocol and unchanged stop verification pass 964,272 sanitizer checks; actual all-ones PGFSM regression is preserved. Early wake stays disconnected. Firmware-first startup now uses independent LSDMA checks (62,190 auxiliary assertions pass), deferring untouched VCN/JPEG under the actual one-use token. The full diagnostic checker is unchanged. |
| Firmware-first memory — root / capture_aux | Source-backed cold-boot audit now recommends deferring untouched VCN/JPEG until ordinary post-PSP initialization. BME-off → MMHUB → BME-on is implemented. Actual platform console capture joins the DCN/firmware reservation union, preserves XNU mapping padding, and rejects unknown virtual-address provenance. 85 sanitizer boundary checks, CMake integration and full native link pass. One-use cold admission is connected through Device to Engine; actual activation remains untested. |
| HUBP scanout | Portable allocator-bound surfaces, pitch/format/address writes, pending/address-in-use completion and bounded blank/drain retirement implemented and added to builds. Fresh host suite passes 1,753,453 checks. Native ScanoutSurface allocates reserved VRAM, verifies full upload/readback, configures blanked pixel/HUBP and retires under normal or callback scopes. The sourced aperture protocol passes 26,376 assertions. No fetch/modeset is enabled; bandwidth/timing/link/console handoff remain. |
| Pixel pipeline | Portable progressive unscaled RGB DPP/MPC/OPP routing, HUBP hardware init and disconnect/idle retirement implemented and added to builds. Fresh host suite passes 407,664 checks. Native modeset integration remains. |
| Display power | Actual DOMAIN0..3 power protocol and native display ownership implemented; portable suite passes 329,865 checks. Native integration now compiles and links in the 65-source build. |
| Display calculations â€” main agent | Twenty unmodified pinned AMD DML32 files calculate real single-pipe clocks, watermarks, global sync, RQ/DLG/TTU and DET. The new integer encoder maps all 70 named fields, binds generations/mode/reference clock and retains four explicit watermark/clock records. Actual 1080p/4K60 calculations and 5,526 encoding checks pass sanitizers on Windows and native macOS. Native source code hashes match; only README changed afterwards. Real board input acquisition and firmware watermark policy remain. |
| Display bandwidth â€” lifecycle worker / root integration | Exact 39-register timing, four watermark sets and bounded DET/compbuf programming are implemented and in all build paths. The controller requires real operational-gate callbacks, all-pipe stop/drain and clock/profile validation. Host protocol tests and kernel-header compilation pass. Native gate, allocation/modeset integration and activation remain. |
| Monitor/high-refresh | Actual 4K165 HDMI timing/EDID retained. EDID parsing passes 2,722 sanitizer checks; FRL training and HPO register protocols pass 3,021 and 1,397 checks and are now in all builds. PHY/DMCUB, FRL stream, DSC, clocks and native ownership integration remain. Negotiated transport/compression is not inferred from advertisements. |
| Metal shader path | Real Apple AIR-to-SPIR-V, validation and C ABI pass; Windows Vulkan established shader semantics. Native macOS and Windows NIR/ACO now emit identical gfx1100 add_one machine code and launch metadata: 104 total/84 executable bytes, 24 VGPR/108 SGPR, no scratch/LDS/relocations. Native compiler/C ABI report and all3,718 Mesa source hashes verified. ACO output has not run on GPU; no macOS Metal device exists. |
| Userspace transport — submission_review | Sealed native VM code/IB and canonical clear/copy admission feed isolated VMID1–7. Completion requires actual fence and RPTR. Strong VM registry now joins Engine's typed reset retirement; cleanup errors remain terminal through factory failure, empty ledgers and retries. Nine current kernel objects compile. No IOUserClient capability is advertised or hardware execution claimed. |

Agent names identify the assignments at this update, not durable live-process
handles. Re-list agents after interruptions and inspect files before resuming.
Workers own their named module files; the main agent owns shared build lists,
native integration and documentation. A worker report is reviewed before its
code is counted as integrated or validated.

Existing baseline code includes actual DMA/GART storage, PSP/SMU/GC firmware
and initialization, graphics/SDMA queue protocols, GFX11 clear/copy dispatch,
MODE1/ATOM reset and typed retirement, DMCUB, EDID, AUX/DP training, HDMI
DDC/TMDS, OTG timing, AVI/VPG and native HDMI stream ownership. These are
implemented protocols with host tests and compilation evidence. None has
demonstrated GPU execution in this project.

## Evidence and its limits

| Evidence | Verified scope |
| --- | --- |
| `build/cross/cross-check-report.json` | Successful compile-only snapshot: 69 x86_64 Mach-O objects, now including display-bandwidth, native VM owner/page tree and Device capture diagnostics. Report does not fingerprint included headers; no loadable kext from this check. Later agent edits require their own validation. |
| `build/macos-live-followup-final/build-report.json` | Frozen 74-source native linked build including new display-probe evidence, normal FunctionGroup gate, isolated submission/completion and FRL/HPO protocols.353 imports resolve, stage removed. This is the source snapshot in `1e4040229efa`; it precedes subsequent VM-registry retirement and early-media edits. No target execution of new code established. |
| `build/macos-audio-console-final/build-report.json` | Earlier 78-source linked snapshot, all358 imports resolved, executable SHA `f21db1150caa1fdbbafee60eacfc148e7c4252036fda3ec31e21f5e05a86b4ce`, UUID `BE61C898-B842-30D6-A2FB-CA95CF97D14E`. 3,696,228-byte source snapshot matches, 7,896KiB stage removed. Earlier `macos-audio-console-integration` exposed an unresolved IODeviceMemory metaclass; the actual PCI resource is now cast to IOMemoryDescriptor. Neither build was loaded. |
| `build/native-submission-final/report.json` | Exact65-input snapshot compiles six real kernel objects and passes6,965 isolated-admission plus3,188 queue ASan/UBSan checks on Intel macOS.483,856 source bytes;2,604KiB stage removed. No VM context or command was executed on hardware. |
| Portable test runs | Prior clean full run passed 50/50 suites in 57.97 seconds. CMake now configures 62 suites; fresh display-smu/display-dpm integration passes 2/2. Fresh cold-aux and hubp-aperture integration passes 2/2; aperture/HUBP/pixel/VRAM sanitizer tests pass 8,480,447 assertions. Latest fresh focused integration runs pass audio-quiesce, early-media and cold-aux (3/3), then EFI-console (1/1). Earlier queue/isolated-compute/FRL/HPO run passes 4/4. New audio/media/console sources have separate ASan/UBSan evidence. This is not a fresh full-suite run; register models do not prove GPU behavior. |
| `build/native-gart-factory-latch-review/report.json` | Nine kernel objects compiled from 68 inputs / 410,603 bytes, with the 812 KiB stage removed. Strong VM registry and GART/factory DMA-cleanup failure propagation are covered. Later BME/console edits are covered by the combined78-source build; no target DMA was executed. |
| `build/native-reset-provider-review/report.json` | ResetController compiles against real Kernel.framework after replacing unbounded/ambiguous PM and PCIe lookup. 25 inputs / 145,033 bytes, 268 KiB stage removed. No reset was issued. |
| `build/audio-quiesce-validation/report.json`, `build/early-media-report.json` | Audio stop and early SMU media protocol pass 1,066,721 and 964,272 sanitizer checks respectively, plus real kernel-header compilation. The exact all-ones cold-aux capture regression passes. Native audio binding has a separate reviewed kernel-header compile; neither path has target execution evidence. |
| `build/macos-native/build-report.json` | 65-source native build: 347 imports resolved, no unresolved symbols, UUID `D0350C0A-16D1-33AF-B8A2-4A0A77759B35`. Includes Device/Lifecycle and display power/HUBP/pixel before subsequent capture-diagnostic/VM changes. Not loaded; stage removed. Embedded `native_bundle` is authoritative for that artifact; older standalone `macho-report.json` in the same mutable folder is stale. |
| `build/macos-live-capture/build-report.json` | Frozen 66-source capture build: 347 imports resolved, none unresolved; UUID `EA44A013-8203-36C3-84E1-523140BA82C9`, executable SHA `6a3a8173c6e25e10a6ed13aadd9690d6d1a2d0bf13cf2a22f465508fa23f1a4e`. Native exit0 and stage removed. Loaded on target with matching UUID as recorded above; later implementation changes are not covered by that boot. |
| `build/display-dml/report.json` / `test.log` | Actual DML32 calculations, input/capacity/unused-RQ regressions and 5,526 integer encoding checks pass ASan, UBSan and float-cast-overflow checks. `build/native-display-encoding/report.json` confirms the same on Apple Clang 17 / SDK 26.2: exit 0, artifacts retrieved/verified, 7,355,099-byte stage removed. All compiled sources match; only README changed since the native snapshot. `build/native-display-dml` remains the earlier result. Reference clocks remain test data. |
| `build/gfx11-compiler/{build-report,test-report}.json` | Real pinned NIR/ACO compiled the Apple AIR-derived SPIR-V to gfx1100. Tests check emitted operation IR, changed arithmetic/local sizes, raw descriptors and strict rejection with no output. New ACO binary has not been executed. Native CLI/C ABI verification passed separately; see `userspace/compiler/VALIDATION.md` and `build/gfx11-compiler/macos-native/report.json`. |
| `build/native-vmcontext/report.json` | Current 111,239-check VM context snapshot passes native sanitizers and real Kernel.framework compilation; all recorded hashes match and stage removed. Prior snapshot is preserved separately. No native page-tree owner or hardware context was exercised in this report. |
| `build/native-vmowner-baseline/report.json` | Frozen 38-file owner snapshot: 128,411 native page-tree sanitizer checks and seven Kernel.framework object compilations passed with Apple Clang 17 / SDK 26.2. Exact stage removed. Subsequent isolated-compute edits are not covered; no hardware context was activated. |
| `build/metal-compiler/test-report.json` / `test.log` | Ten compiler tests, four Windows Vulkan GPU tests and dynamic C ABI passed, including genuine Apple AIR. Native compiler/AIR reports live in `build/metal-compiler/macos-native` and `apple-input`; Windows AMD ICD execution establishes shader semantics, not HackyNavi or macOS Metal execution. |
| `build/native-transport/report.json` / `build.log` | Current six source/header hashes match; real macOS IOKit dylib/probe linked and native sanitizer protocol tests passed. Source stage was 40,284 bytes; remote stage removed. No installed service or GPU call was exercised. |
| Frozen native evidence in diagnostic package | Unsigned 61-source native build: Apple Clang 17 / SDK 26.2 on Intel macOS 15.8 (24H22), 343 kernel imports resolved. Includes earlier native clocks/idle/PCI work, not current power/HUBP/pixel or sleepable lifecycle work. Full source archive and build report are packaged and their hashes verified. |
| `build/macos-sanitizers/report.json` | Successful display-SMU ASAN/UBSAN run: 5,995 checks; earlier display-idle: 43,608. Later power-aware idle and new modules are outside this result. Remote staging removed. |
| Target runtime | Both frozen passive and live-capture packages loaded on15.8.1/24H32 with matching UUIDs. Live capture validates discovery/runtime ROM/memory and generation3, but reports DOMAIN0 power classification failure and VCN0 PGFSM all ones. HDMI/no incident reported by user. No GPU execution, controlled modeset, hotplug, PM recovery or Metal result. |

The native baseline has SDK libkmod deployment warnings and read-only kmutil
build-folder ownership errors; dependency resolution succeeded. The mini's
macOS 15.8 differs from the intended 15.8.1 target. Reports and logs under
`build/` are local ignored artifacts; another checkout must rerun validation
or receive the reports with their source manifests. File timestamps or an
updated source archive do not turn an older report into current evidence.

The new native build finished with exit 0 and removed `HackyNavi-build.YXPSP4`.
Its executable SHA-256 is
`4bd69fb2953a2bddeda12ba86001339fdb26b587b4bc955becd9486a6817fc89`;
source archive SHA-256 is
`fc085190bdc208031b1d599b672d99b986455621c9c3a0e4e9eb16ad973519d7`.
This development artifact is distinct from the first-test release. Preserve
that immutable release and its successful target evidence.

The separate live-capture ZIP is 2,148,638 bytes; SHA-256
`ad32687b60c3f1c8e0d05ee9876115f2f1ba97a316ef60f1a7fe8e91e6fca93b`.
The packager verified all15 payload files against the ZIP and the native source/
artifact archives against their manifests. It refuses to overwrite releases.
The analyzer now supports explicit `--mode live-capture`, requiring matching
generations and current validated capture; historical observations never confer
current hardware authority. Original passive evidence still passes; synthetic
analyzer positive/negative checks test the analyzer only.

Both reported native runs completed and removed their staging directories;
the abandoned source-only staging directories were also removed. The frozen
diagnostic ZIP is 2,081,624 bytes, SHA-256
`2bda3f94bc5ad401700d7e7bf522a9b511871dcee1e0e244674ecfb519c456fc`.
Executable SHA-256 is
`dd4f86a48836ca83667140fadd846e4a6b98f62eefd6be00d199a874991f6d47`.
All 15 package files were verified against the ZIP; source/archive/binary
hashes match the native report. Collector shell syntax passes. These checks
were packaging evidence. Subsequent target logs now establish loading and
passive observation for this binary only; the package remains fixed while
development continues. The analyzer checks raw runtime evidence rather than
rewriting historical `load_tested=false` metadata in the original package.

## Next integration and unresolved dependencies

1. Finish native power/modeset and lifecycle integration review; portable HUBP,
   pixel-pipeline and display-power modules are now in all build/test paths.
2. Bind implemented userspace DML32 calculations and integer encoding to real
   board clocks/latencies, firmware watermark policies and native programming. Do not insert
   guessed constants or treat the test reference profile as target measurements.
3. Combine owned scanout allocations, clocks, pixel PLL, OTG, pixel pipeline
   and HDMI/DP streams into a native modeset transaction with failure retirement.
   DP still requires pixel stream/MSA/transport programming beyond link training.
   Full 4K165 requires HDMI FRL and DP DSC/high-refresh planning; keep HDR deferred.
4. Implement bounded engine stop/reset and power-loss behavior, HDMI-audio
   sibling reset exclusion, and global display/firmware/PM exclusion before
   activating engine startup from the PCI service. XNU error ACKs or delayed
   ACK timeouts cannot serve as a power-removal veto.
5. Implement framebuffer/IOSurface, isolated user submission and synchronization,
   scheduling/recovery, Darwin RADV/ACO transport and the actual Metal frontend.
6. Produce a traceable test bundle with source/artifact hashes, exact opt-in
   behavior, expected diagnostics and recovery steps. Then verify staged GPU
   execution and physical outputs on the target, followed by Metal/presentation
   and lifecycle testing. No completion claim before that evidence exists.

The full scope and completion criteria remain in PLAN.md. This status file
does not redefine success around the modules already implemented.
