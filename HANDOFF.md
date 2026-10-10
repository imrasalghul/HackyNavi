# Build and development handoff

Latest integration: [combined graphics preparation](docs/validation-2026-10-11-combined-graphics-evidence.md).
Full native core/aux and updated Metal/tool builds pass; early recovery, paired
function and partial display/DMA evidence are now retained. The next target
test remains withheld until inherited-framebuffer ownership is implemented.

Updated 2026-10-11. Start with [STATUS.md](STATUS.md) and [PLAN.md](PLAN.md).
The user explicitly requires care with storage on `victor@mini`.

Latest user constraint: no further probe-only target reboot. Audit every
first-graphics prerequisite and batch all required preflight/startup/Metal and
failure observations into the next graphics test. Do not issue a new capture
package or ask to repeat 3fa. The next test must attempt real GPU work; it must
not bypass an unresolved ownership check to satisfy that requirement.

The supplied `live-3fa3f6d4723b-20261011-163144-730` capture passes exact UUID
and live-capture checks. All 32 context words completed two unchanged passes.
Pipe0 timing is active; timing1–3 is stopped with HUBP functional clocks
disabled. Source-backed analysis still leaves active TLB0 mapping and dormant
requester retirement open. Retained originals/manifests/analysis are in
`build/target-evidence/live-3fa3f6d4723b-20261011-163144-730`; detailed source
interpretation is `build/target-3fa-display-review-1/report.md`.

Current assignments: the native PSP and DP authors cross-review the completed
recovery/function-group and raw-read evidence patches; the audit worker investigates
the actual physical console mapping owner and exact supplied-ROM GOP behavior. Combined
preflight, CP/SDMA inventory and Metal admission reporting now pass their native
builds. PSP/reset source and DP stream implementation have passed focused
validation. Root owns shared lists,
runner/collector, integration and SSH. See `docs/next-graphics-test-audit.md` for
the complete combined-test dependency/collection audit and remaining gaps.
The issued capture ZIP must remain byte-identical during these changes.

The accelerator-port client route and real v1.7 vertex-buffer kernel
admission/draw/capture/Context and Metal bindings are implemented. Native
transport, compiler/runtime, 22-unit frontend and four target-tool builds now
pass, with independent source/binary/package reviews. All remote stages were
removed. No target GPU or Metal tool has run. Typed PSP HDCP/DTM protocols are
compiled and native controller invocation/retirement is implemented; full native
link passed, while hardware validation remains pending. Separate read-only
boot-display context capture is integrated into the new native core. See
`docs/boot-display-address-audit.md` for the two still-rejected target conditions
and `docs/metal-vertex-buffer-render.md` for the bounded VBO scope.
Keep the tested 9eda package immutable as rollback. Its target load/capture
test passes, with exact UUID verified in the supplied 15:33 capture. Its raw
display registers fail the inherited address decoder; establish the actual
scanout memory reservation before giving an engine-enabled test. See
[new target evidence](docs/validation-2026-10-11-9eda-loader-success.md).

Latest completed capture test, built 2026-10-11 at 16:05 America/New_York:
`build/releases/display-context-20261011/HackyNavi-live-capture-3fa3f6d4723b.zip`, SHA-256
`01d9308042ce78a6e98bff88787f97a01242c9390c87f7ab83b650fca5309e8a`.
Its own HANDOFF.md is authoritative: `-hackynavi-device`, preserve `-radvesa`,
engines absent, current HDMI, collect complete logs and OpenCore debug output.
The adjacent `OPENCORE-AGENT-PROMPT.md` gives the installation-session handoff.
USB contents were not changed by the driver session. This build adds 32 raw
display-context registers in two passes; retain all masks and partial captures.
It grants no display ownership or engine readiness. Package verification:
`build/vertex-context-package-review-1/report.json`, SHA-256
`f1913f81a1aa584f9f6fd536fac53439029f4b425d5a9cc86d81fa6613a311a2`;
the adjacent agent prompt is also independently reviewed in that directory.

Earlier combined-preflight native build: `build/macos-combined-graphics-preflight-1`, finished
20:57:59 UTC; 113 core units, two accelerator units, 285 archived inputs, all 413
core imports resolved from the build host's BootKC. Report SHA-256
`183421b507ce3522638f065ac18216126bd28a592d02ed76cc951e1452868d73`.
Core SHA-256 `f94b8eb5d4260607c09fc552d9027d5686ce25e52926212055c29ee7e093d3bd`,
UUID `9C8CE74E-1FB8-3FB9-86FD-EEBBE6BA67F9`. This is not a test release;
subsequent evidence/ownership edits need a new frozen build. Updated Metal frontend
and all four tools: `build/graphics-admission-frontend-native-1/report.json`, SHA-256
`e2c2e62a1b0bef9f55a287cd2bb92f9352097b7931da1b358cdfb68198255e91`.
Both remote stages were removed; no GPU work, installation or loading occurred.

The issued `build/macos-vertex-display-context-1` capture build has 112 early-core
units, two late-accelerator units, 283 archived inputs, ten real firmware images.
All 412 core imports resolve from BootKC; auxiliary imports resolve from their
actual collections. Core executable SHA-256
`3fa3f6d4723bdd08e28de0f8ce49b6c0f3a7af87013f3a6c0e87e66b9d7fbfc5`,
UUID `F2DDA1C3-D4B2-3C4E-AF63-E70C25298DAC`. The 11,320 KiB stage was removed.
The actual auxiliary bundle remains separate; never put it in OpenCore
Kernel/Add. Late deployment, actual engine boot and Metal execution remain
unverified. No kext loading, Metal device creation, installation or GPU execution
was performed on mini.

c39 failed at OpenCore injection: IOGraphicsFamily missing from BootKC, vtable
patch failure and Invalid Parameter. The split and independent review correct
that dependency layout; do not repeat unchanged c39. See
[loader evidence](docs/validation-2026-10-11-c39-loader-failure.md) and
[late accelerator contract](docs/accelerator-late-loading.md).

Previous kernel: `build/macos-fragment-buffer-integration-1` has 109 translation
units, 414 resolved imports and 271 independently verified archived inputs.
It includes the actual fragment-buffer admission/draw, retained native capture
and v1.6 wire path. Executable SHA-256
`c39afcee43f96070f8a230f11ff890360cc6edbe2bc2bb7ab67ef743b9560180`,
UUID `100496F0-532E-374A-B4A4-3D6E0D61E3D0`. Independent report is
`build/macos-fragment-buffer-integration-1/product-wire-independent-verification.json`.
The 10,444 KiB stage was removed. Native transport compilation and 160,054
ASan/UBSan protocol checks passed in `build/native-buffer-transport-1`; its
1,184 KiB stage was removed. Context's actual retained buffer draw and the full
22-unit Metal frontend now compile/link in `build/metal-buffer-native-1`, with
45 native commands, six exports and three target tools compiled only. Its
62,302,702-byte stage was removed. Bundle executable SHA-256:
`8371b4998c843245f29e45c53cadcb0338566321118327391047f091443274b5`.
No GPU,
Metal object or kext was run on mini. See
[buffer path](docs/graphics-fragment-buffer-native.md) for exact scope.

Previous kernel: `build/macos-boot-backend-integration-2` has 109 translation units,
414 resolved imports and 269 archived inputs. It includes the real read-only
inherited HDMI backend observer after DMCUB stop and distinguishes unsupported
profiles from changing/failed hardware. Root verification is
`build/boot-backend-native-parent-2/parent-verification.json` (SHA-256
`f9e60412ac1e58f31ff4480553722cc7b391797273da4c98f1555fed520fb34a`).
Executable SHA-256 `793a149327ae87225b20c786df572defb78614a75c38e3a181f1c1f8e1e5b6ad`,
UUID `F97274D1-EE9C-3D7E-9DEB-F9B14F1F9168`. Remote stage removed. No load,
display transition or GPU execution. Integration-1 is superseded by the precise
IOReturn failure mapping in integration-2. The OPP blank transaction is separately
implemented/reviewed in `build/boot-opp-blank-local-1`, without a native mutating
callsite. Separate MPC/OTG stop passes `build/boot-backend-stop-local-1` and
`build/boot-backend-stop-cmake-integration-1`; complete inherited HDMI ownership
remains in progress. Native observer frame evidence including the final failure
mapping is independently bound in `build/boot-backend-native-vcn-review-3`.

Latest userspace package: `build/metal-vertex-development-package-2` contains
137 files, 4,394 source members and 15 Mach-O images. Archive SHA-256
`e0d101fe275a7621a0056560df68881bcd0319176ad5ef02ab638c0cde93f79d`,
38,745,093 bytes. It binds `build/graphics-vertex-buffer-darwin-native-1`
(270 compiler checks, 16 genuine AIR/metallib products, six graphics exports),
`build/graphics-vertex-runtime-native-1` (96 ASan/UBSan checks, eight pipelines),
and `build/vertex-frontend-native-1` (22 frontend units, four compiled target
tools). Validation rejects 24 malformed binary variants. Independent source
and package review: `build/vertex-graphics-independent-review-1`. This package
is not part of the next boot-capture installation and has not run on the target.

Previous userspace package: `build/metal-buffer-bundle-package-1` contains 130
files, 4,343 source members and 13 native images; package/mutation checks pass
in `build/metal-buffer-bundle-package-check-1`. Archive SHA-256
`c46b6d6c6fa3a0474f4002dc3914750ee61f7bbe5beab700876a6d6f1c3e14e8`,
36,569,742 bytes. It binds `build/graphics-buffer-darwin-native-1` (88 real
compiler checks/twelve products, exact four graphics C exports) and
`build/graphics-buffer-runtime-native-1` (54 ASan/UBSan checks/four real pipelines).
All remote stages removed. No device discovery, installation or target execution.

Previous userspace integration:
`build/metal-graphics-native-2` passed all 21 frontend compiles plus full
bundle/dylib linking and both target-test compiles (41 native commands, no
diagnostics, five exports). `build/graphics-darwin-native-4` passed the actual
52-check stage compiler and 493-check color-layout tests with correct C exports.
Both stages are removed. Native C++ runtime integration subsequently passed
24 actual compiler-backed ASan/UBSan checks in `build/graphics-runtime-native-1`,
with six exports and its stage removed. The matching package is
`build/metal-graphics-bundle-package-final-1/HackyNaviMetal-development.tar.gz`
(34,500,328 bytes, SHA-256
`dd501a13929d4f4145fddc1daa3ffd40be8a6e0afbcbd2f1b78a5a4a7d7c34ec`).
Root verification in `build/metal-graphics-package-parent` checks 124 files,
4,331 source members and 11 native images. Target tests and fixtures are under
`Contents/Resources/Tests`; they have not run. Bundle executable SHA-256:
`e74c5be08b545ce292cdaf34742713cc91b41e3d14058ac7ab2a9c33537ef707`.
This is build/offline compiler evidence; no target Metal execution occurred.
Independent combined frontend verification is
`build/metal-graphics-parent-2/parent-verification.json`; final render-path source
review is `build/metal-render-path-display-review-1/independent-review.json`.
Current worker ownership: product establishes real PSP HDCP/DTM ownership after
completing the reviewed AVMUTE/frame-cycle/AFMT transaction; display validates
the real fragment-buffer compiler and immutable POSIX runtime builder; VCN
connects Metal buffer binding/recording/preflight/execution and its target-only
smoke. Root integrates and reviews the native kernel/Context path, shared build lists and all
serialized native jobs. Do not instantiate Metal objects or run target tests
on mini. The already-issued live-capture diagnostic remains the target package;
these development builds do not replace it automatically. Runtime MSL compilation
needs a truthful `featureProfile` contract; see the final source-compiler audit.

`build/macos-graphics-draw-metal-y-1` is the latest 107-unit native kext build:
Metal Y correction included, 413 imports resolve, stage removed. Independent
verification is `build/graphics-metal-y-parent/parent-verification.json`. It is a
development artifact, not an issued target test. The Metal pipeline/texture/draw
command source is wired, reviewed and passes native 21-unit compile/link.
Use the new target-only `userspace/metal/native/render-smoke.mm` for eventual
actual readback verification; compile only on mini. The independent tiny stage
and full frontend builder are separate so compiler runtime linkage cannot hide
Objective-C SDK errors. The compiler-native1/2/3 failure evidence is preserved;
the successful fix separates addrlib's Apple builtins from the extra Zig runtime.

Previous integration evidence:
`build/native-graphics-capture-local-3` freezes atomic real DMA graphics capture;
`build/gfx11-graphics-draw-local-4` is the reviewed corrected 712-DWORD draw
encoder (local-2 contains a preserved GFX10 color-register defect). Topology,
VA placement, never-started queue retirement and no-queue RLC retirement are
reviewed. Explicit native v1.5 negotiation is enabled. The 107-unit full build in
`build/macos-graphics-draw-integration-1` passed with 413 resolved imports and
root-verified archived inputs/bundle. It predates the newly found Metal viewport
Y correction; do not ship it as a graphics test release. No target draw occurred.
Metal texture, render pipeline, shader and command code is now written, with
combined native Objective-C compile/link and independent command review pending.

`build/native-graphics-transport-1` compiled/linked the actual new IOKit transport,
ran 159,950 ASan/UBSan protocol checks, exported five binaries and removed its
1,128 KiB stage. Root verified all 14 source members and artifact hashes. No
connection/GPU test was run. `build/runtime-graphics-local-1` now covers explicit
openGraphics, immutable graphics pipeline and real draw methods. Shader evidence
is `build/runtime-graphics-shader-local-1` (use additive freeze2 for Library init
correction); texture evidence is `build/metal-texture-local-2`. Native compiler
export validation failed in `build/graphics-darwin-native-1`; the stage was removed
and the compiler worker is preparing a real Apple relink. Shared build lists and
the mini build lane remain root-owned. Keep all superseded evidence.

Latest frontend: `build/metal-device-native-2` passed 14 native compiles and
31 total commands without diagnostics. Root verified 40 archived inputs, all
14 static-archive objects and four exports; stage removed. Actual MH_BUNDLE
`HackyNaviMetal` SHA-256:
`837ee6bc0e57cabc235b104fae37965d3e411828de40030e6cdb5055ef18cacc`.
Static Objective-C checks and packaging are complete in
`build/metal-bundle-package-final-3`: all 391 own required method encodings,
88 package files and 4,198 source members verified. Archive SHA-256
`c78cceec88dfb01d1337a8576fab3d573fbae036482cb6af39e3704599aecd43`.
No installation, device construction or target execution yet.

Prior kernel: `build/macos-graphics-clock-integration-1` contains 102 translation
units, 252 verified input files and 412 resolved imports. Root verified the source
and artifact archives plus the real Mach-O UUID and symbols. Executable SHA-256:
`0624ac849a0a33d8654d9fab813d4a416ef25ea07f850033d01fc31534faaac1`;
UUID `37C226DC-23DD-381D-81EF-B605EB266468`. The 10,116 KiB stage was removed.
This development build has not loaded or executed engines on the target.

Combined native check: `build/native-graphics-clock-integration-1` passed eight
ASan/UBSan suites (41,992,134 assertions), 19 kernel objects and 119 archived
input hashes without diagnostics. Its 6,444 KiB stage was removed. This covers
native cold DISP/DPP ownership, power guards, actual GART color-topology capture,
shader/color validators and the GART owner workspace stack correction.
Independent graphics ISA and topology reviews are complete in their local
freeze directories. The compiler Apple graphics fixture is also reviewed.
Do not activate the unfinished startup transaction or substitute this binary
for the previously issued live-capture diagnostic.

Workers at that earlier milestone: product owned atomic native graphics capture, VA placement and
live harvested topology; display owns the complete sourced GFX11 draw encoder;
recovery owns genuine never-started Prepared queue retirement. Root owns shared
build lists, independent integration review and the serialized mini build lane.
Existing sources were frozen for the reports above and then released to workers;
subsequent work is not retroactively part of those verified artifacts.

Use the fixed-system AIR helper from `build/metal-system-reader-native-final`
(SHA-256 `9d5428451f07fb8789b92ae8500c1a29a7a6b924b0a9b68699f3df0acb79655e`)
with the same verified compiler. Its six native commands and 94 ABI checks pass;
46 input members were verified and the stage was removed.

Latest compiler: `build/metal-explicit-tools-native-final-2` passed 94 explicit
C ABI and 27 genuine C++ ASan/UBSan checks with 32 exports. Root independently
verified 44 archived inputs, 22 commands and both exported files. Dylib SHA-256:
`1be08ab74e61e05ebfe49caf739f8986e118740f8e0ece4938534b2cb6d85e2d`.
Stage removed. Use this report with `userspace/runtime/build-frontend.py` when
building the current explicit-tool Device/Library. The previous components-only
snapshot below predates this ABI. Never execute the target smoke on mini.

Root's subsequent DF policy source is in `build/smu-df-cstate-local`:
153,589 strict/UBSan checks and clean independent review. The native run in
`build/native-smu-df-cstate-final` passes the same ASan/UBSan checks plus four
kernel objects; root verified 88 inputs and the 2,616 KiB stage was removed.
It owns explicit
DISALLOW and terminal ALLOW (no prior-policy restoration), with cold checks
and retained failure authority. The later complete frontend and package
supersede this component milestone as described above.

Current step: real Managed Metal CPU/GPU copies and ordered readback passed
independent source review. Root owns `HNMetalBuffer`, runtime allocation
accounting, `compute-smoke.mm`, and the bounded `build-frontend.py` runner.
`build/metal-managed-prepared` freezes eleven actual components before the
next compiler ABI change; device/capability integration is explicitly excluded.
Its native build passed at `build/metal-managed-native-1`: 22 commands without
diagnostics, 32 verified inputs, eleven verified static archive objects and
three exported artifacts. The dylib SHA-256 is
`6d143763b9765ee83ee225a75938f234d96785cae4e1660cfded132346878a6f`;
the target smoke SHA-256 is
`77217462c507bef01f85f9e23aa91dfa1839906bc5a2cdc1fa7725989c9d12dd`.
Stage removed. The current runner adds nonempty-output protection and a final
storage measurement; these were not retroactively included in that build.
The target smoke is compiled only on mini and uses public Metal discovery,
managed A-to-B-to-C dispatches, partial groups, guard checks and callbacks.
Never execute it there. Display worker owns device/library/command integration,
capability worker owns measured limits, and compiler worker owns explicit
per-compile bundled helper paths. No plugin may set application-global env.

Newer kernel work: SOC/Fabric deep-sleep inhibition/restoration passed 138,600
native sanitizer checks plus four kernel compiles in
`build/native-smu-deep-sleep-final`; root verified all 88 archived files and
native object log hashes in `parent-verification.json`. Its 2,548 KiB stage
was removed. This is now included in the latest complete kext below. The later
DF policy source is described above; full clock-transaction integration remains.

Prior full kext (superseded by the 102-unit build above): `build/macos-engine-display-retirement-final`, 98 translation
units, 412 resolved imports, 245 verified source files and six bundle files.
Executable SHA-256:
`e242c1b221a133bcad72f8f199572bb48e18c59c924feef7d4ac5e05ef334c42`;
UUID `7C04C291-FB69-3E7B-A8CE-68285CE168B4`. The 9,976 KiB stage was removed.
This includes clock controls, ordered Engine/Display retirement and the barrier.
`parent-verification.json` preserves the no-load ownership errors and SDK
warnings; no target load or GPU work is established. The user-issued diagnostic
45eab551a719 remains a separate immutable package and has no returned boot logs.

Previous full kext: `build/macos-accelerator-final`, 94 translation units,
412 resolved imports, 239 verified source files and six verified bundle files.
Executable SHA-256:
`12633219109b34fd9440e07bb2662cf462595bc742e18f9ed2aa6ae908bd4065`;
UUID `0E7454F6-F2F4-33E6-B126-541F99FC1252`. The 9,868 KiB stage was removed.
It includes the public accelerator child and FAMS/DAL coordination. See
`post-export-validation.json` and [service ownership](docs/metal-accelerator-service.md).
No target load or Metal execution occurred. Later live-DMCUB UCLK admission
passed review and kernel compilation in `build/native-fams-uclk-final`:
object SHA-256 `69d9467382677df67be472061884f7962a8883f24d6713754e9d619595e684a3`,
604 KiB stage removed, no unchanged protocol suites repeated. This bridge is
now included in the latest full kext above and has not run on hardware.

Latest display evidence: `build/native-fams-final/report.json` records
29,370 native ASan/UBSan checks and three kernel objects with a 2 KiB frame
limit. All source archive members were verified and the 1,992 KiB stage was
removed. `build/fams-local/independent-review.json` covers the actual deadline,
ring ownership, DAL-before-FAMS order and retained partial-policy cleanup.
There is no startup/modeset caller yet. The latest full kext above includes
this work. Root has integrated the public accelerator service; Metal workers
have natively linked command objects and are implementing the real device
frontend and measuring bundle trust requirements. No target GPU test occurred.

Latest userspace evidence: `build/native-compute-command-final/report.json`
records 55,881 ASan/UBSan host lifecycle checks, strict ARC command-object and
C++14 scheduler compilation, and an eleven-object runtime link. All 17 native
commands passed without stderr. Dylib SHA-256 is
`c128ccb0c12ad50c0501cc98f3014a2e2c41d351b7f32133ddf70e86063276b0`.
Parent verification in `build/compute-command-parent-verification.json`
checked 37 source archive members, six exports and nine byte-identical reused
objects. The 42,435,464-byte Mac stage was removed. No actual Metal object or
GPU Context was created. Preserve the superseded local-1/-2 reports: final
local-3 corrects blocking capacity, releases execution capacity before
callbacks, and waits for scheduled handlers separately from completion
handlers. Continue with real HNMetalDevice and trusted bundle discovery.

Preceding pipeline evidence:
`build/native-compute-pipeline-final/report.json` records strict ARC compilation
and linking of `HNMetalComputePipeline` plus 265 native ASan/UBSan checks across
twenty actual shader variants. Runtime dylib SHA-256 is
`722af6710c50549769135cd1b4e85f30e2347b10e298b6bc92076dd203af505c`.
It uses `build/metal-compute-metallib-native-final`, compiler dylib SHA-256
`1718d24dc9a08896d50c140ef5fe418d40769404ad184d177212ef693954f943`:
460 existing owned ABI checks, 48 new real compute-metallib checks and exactly
31 exports. Both stages were removed. Parent verification independently
checked all 75 archived inputs and eight exported artifacts; see
`build/compute-components-parent-verification.json`. No Metal object was
constructed and no GPU connection opened. Queue/buffer/encoder scheduling and
the public-accelerator discovery route are the next parallel work.

Previous library/function milestone:
`build/native-compute-shader-library-final-2/report.json` records native ARC
library/function compilation,252 new Shader ASan/UBSan checks and verified
exports. Runtime dylib SHA-256 is
`cc949c72a7f8adaacf0189bb50895e889afa185c734da68a3e3c9ac364f74f25`.
It uses the31-export compiler in `build/metal-metallib-owned-native-final`,
dylib SHA-256
`876680e8255777987cbbe9a1c9b3acf42047a8c7281658bcb2cf7fa95ceb25cc`.
The prior five runtime objects were reused with exact byte verification.
Neither run constructs a Metal object or opens the GPU. All temporary stages
were removed. Keep the failed first compile and its pedantic-only correction
as historical evidence; do not overwrite frozen reports.

Source-derived register occupancy is now integrated into native ISA admission.
`build/native-gfx11-occupancy-admission/report.json` records 471,170 native
ASan/UBSan checks and one real-header kernel object compile. Source snapshot
and object hashes are recorded there; the 1,020 KiB stage was removed. The
`macos-compute-admission-final` full kext includes this change; the issued
diagnostic package predates it.

Further native admission work is verified in
`build/native-gfx11-uniform-admission/report.json`: 99,961 core and 120,732
direct dispatch sanitizer checks, a real-header kernel object compile and
verified removal of the 1,076 KiB stage. Source-derived scalar integer,
lane-transfer, uniform MUBUF and packed local-X instructions now admit actual
one-thread and multidimensional compiler products. No full kext rebuild or
target execution is implied. See `docs/gfx11-uniform-compute-admission.md`.

Current follow-on: real command objects and accelerator discovery. The Mac's
default `metal` launcher fails, but explicit selection of installed toolchain
`com.apple.dt.toolchain.Metal.32023.864` succeeds; see
`build/metal-mounted-probe/report.json`. No install/download was needed.
`build/apple-compute-metallib-capture/report.json` binds a real offline-compiled
macOS15/MSL2.4 AIR and MTLB2.8 file, exported before its 7,331-byte stage was
removed. Parser ownership/compilation now passes native checks in the latest
compiler report above. This is a compiler fixture, not a Metal source-library
API or construction of a discovered Metal object.

Migration was deferred by the user before completion. The active checkout is
still on Windows; this document is a prepared handoff for a possible later move.
Do not assume the described Mac checkout or migration receipt exists.
Development has continued since packaging: regenerate any future migration
snapshot from the current working tree. The prior package is stale.

The passive boot and subsequent explicit live-capture boot both ran on the
target. The latter used the frozen `HackyNavi-live-capture-6a3a8173c6e2` package,
`-hackynavi-device`, and the existing `-radvesa` argument. See
[the live results](docs/validation-2026-10-11-live-capture.md) and its immutable
[package instructions](build/releases/HackyNavi-live-capture-6a3a8173c6e2/HANDOFF.md).
Follow-up capture changes preserve the missing raw display power value;
the engine path is still not activated. Installation work belongs to the
other session. Do not load on mini.
The new frozen package is
`build/releases/HackyNavi-live-capture-1e4040229efa.zip`; use its own
[instructions](build/releases/HackyNavi-live-capture-1e4040229efa/HANDOFF.md)
for its capture procedure. It was built from `build/macos-live-followup-final`,
before the later VM-registry retirement changes. Do not substitute a mutable
development bundle. The user has now supplied a distinct matching-UUID boot
for this executable at `E:/HackyNavi-diagnostics-20261010-212355`; all 14 files
are preserved under `build/validation/live-capture-1e4040229efa-20261010-212355`.
All twelve commands succeeded. DOMAIN0 status is `0x10000000`, CONFIG and the
confirmation were not read, and VCN0 again returned all ones. See
[the follow-up result](docs/validation-2026-10-11-power-capture.md). Do not
repeat the same package simply to retrieve the value we already obtained.
Real Metal and bootable-driver integration are the active priority; HDR is
explicitly deferred.

The subsequently tested capture is frozen at
`build/releases/HackyNavi-live-capture-dbbcb5ec0ba9.zip`. Follow
[its own handoff](build/releases/HackyNavi-live-capture-dbbcb5ec0ba9/HANDOFF.md).
It adds the source-audited conditional FORCEON capture and all four power
probes, plus raw display-register reporting after stable capture. Exactly
three files differ from the prior tested capture snapshot; later VM changes
are excluded. Native build: 74 units, 353 resolved imports, 167 verified source
files; focused native ASan/UBSan: 5,033,482 assertions, zero failures. Both
mini stages were removed. UUID is `49B982AA-6376-3D09-B101-CB5B6B478A6E`;
executable SHA-256 is
`dbbcb5ec0ba932a667d3fe295baf59939753246c4de983e316889263f1f9997c`.
This exact package has now loaded with matching UUID in the user's
`E:/HackyNavi-diagnostics-20261010-232322` submission. All four domains have
stable `0x10000000`/CONFIG0, disproving the earlier FORCEON hypothesis. See
[new evidence](docs/validation-2026-10-11-config0-capture.md). Do not repeat it
to retrieve CONFIG again. A selected five-file current-FSM/VCN-prerequisite
follow-up is frozen at
[`build/releases/HackyNavi-live-capture-45eab551a719.zip`](build/releases/HackyNavi-live-capture-45eab551a719.zip).
Use [its handoff](build/releases/HackyNavi-live-capture-45eab551a719/HANDOFF.md).
All 74 units built, 353 imports resolved, 167 sources and 15 package files
verified; both mini stages were removed. UUID is
`564B24EE-00F5-35C0-8071-38310097F55A`; executable SHA-256 is
`45eab551a71944458830b5e3c7b7d44142c65fb4ba641f6dfe922c9a6e22f9f9`.
This new exact package has not been booted. Preserve
`-hackynavi-device` and no `-hackynavi-engines`. The user's OpenCore agent
handles USB preparation; this session has not touched the media.

Latest frozen combined native build: `build/macos-compute-admission-final`, 93
translation units,407 resolved imports,237 verified source files and six
verified bundle files. It adds register occupancy/uniform/multidimensional
ISA admission and the reviewed CPU-only task-memory owner (no client callsite).
Executable SHA-256 is
`cdd9f0fa87442493c9e7b0286c0b0a718e884c43bcdcbf81d68ffd7977b82207`,
UUID `95A90410-1BE8-33C1-AA4B-DD6E3C2C968C`; the9,836KiB stage was removed.
Its no-load diagnostic retains the staging ownership errors, and it has not
been loaded on the target. The later FAMS/DAL coordination work is outside
this frozen snapshot. The issued capture ZIP remains unchanged.

Previous frozen combined native build: `build/macos-reusable-v14-final`, 92
translation units, 406 resolved imports, 234 verified source files and six
verified exported bundle files. It includes the reviewed v1.4 reusable
Session/UserClient and result-history interface. Executable SHA-256 is
`67fc0db187d72a5fd459194924d0f17b91dd6f20c3a08c69eee6d19863ea8cb6`,
UUID `57C5BBE4-4867-34FA-B4E6-1E3F7F6656CB`. Its 9,808 KiB stage was removed;
the separate three-object native Session check also passed and removed its
616 KiB stage. Exact evidence is in
`build/macos-reusable-v14-final/post-export-validation.json` and
[validation](docs/validation-product-wire-power.md). It has not run on the target.

The preceding `build/macos-reuse-final` milestone has 92 translation
units, all 406 imports resolved and 234 archived source files verified.
Executable SHA-256 is
`fe01c5be0f8838dd238fd079cf7709324a81a69d13254e9590f5c50ec854f66b`,
UUID `62487AB0-90FD-3668-B0D3-0DE389D35EEA`. Its 9,792 KiB stage was removed.
Checked VM reactivation, native completed-job consumption and the internal
two-job startup probe are linked. Focused native ASan/UBSan passed 241,988
VM-context and 5,202 queue checks; eight objects compiled with the 2 KiB frame
limit, source hashes matched and the 3,820 KiB stage was removed. Exact archive
and build hashes are in [validation](docs/validation-product-wire-power.md)
and `build/macos-reuse-final/post-export-validation.json`.
Neither this kext nor its two-job probe has been run on the target. The later
v1.4 Session/client/result-history implementation is outside this snapshot.
Independent Session/UserClient and transport reviews found no defect; strict
and UBSan protocol tests each passed 158,417 checks and three real
kernel-header objects compiled. Its `--reuse` supplied-source smoke has
object-compile and actual-client compatibility evidence. Native transport
validation then linked the IOKit dylib and four tools, passed 158,417 ASan/UBSan
checks, verified all five exports and removed its 1,040 KiB stage. The v1.4
full-kext snapshot above is complete; target execution remains pending.
Existing profiles remain single-job.
Keep the immutable 45eab capture ZIP as the current diagnostic handoff.

New userspace `compute::Context`, `Buffer` and immutable `Pipeline` source is
separate from that kext archive. The target-only
[Context smoke instructions](userspace/runtime/CONTEXT-SMOKE.md) describe its
actual A->B->retained B->C test, linked compiler/tool dependencies and explicit
`--run` gate. Native runtime build/link and 233 actual compiler-backed sanitizer
checks passed in `build/native-compute-runtime-final`; all six artifacts were
verified and the 60,083,757-byte stage removed. Strict ARC compilation of the
private `HNMetalBuffer` passed. Its later exact-context identity correction
passed the focused `build/native-compute-runtime-identity-final` relink; four
tested C++ objects remained byte-identical, four artifacts were verified and
the 40,960,697-byte stage removed. Do not execute the GPU tool on mini or treat
compilation as a Metal device registration test.

The additive owned metallib ABI is frozen in
`build/metal-metallib-owned-local-2/source-freeze.json`, with 19 Rust tests,
460 actual DLL ABI checks and all seven real SDL functions compiled from AIR
copies after library/input release. Existing compiler/CLI behavior passed;
root review found no defect. Native validation completed in
`build/metal-metallib-owned-native-final`: all 18 commands and 460 owned ABI
checks passed, exactly 31 public exports verified and the 158,332,943-byte
stage removed. Dylib SHA-256 is
`876680e8255777987cbbe9a1c9b3acf42047a8c7281658bcb2cf7fa95ceb25cc`.
The old immutable 23-export dylib remains distinct. Native CLI and GPU
execution were not performed.

The compiler's separate native metallib integration completed in
`build/metal-metallib/native-run-2`: 17 commands passed, seven actual SDL
functions matched raw AIR and independent SPIR-V validation, 25 negative
cases rejected, all 23 exports verified and the 158,219,947-byte stage removed.
Dylib SHA-256 is
`7d12193d9dde38cd97ba16ec241d1c4a6ad35e90b36b604dc964da0901ac7d24`.
Native CLI was explicitly skipped; separate local CLI and 15 Rust library
tests passed. The fixture establishes the measured MTLB 2.5 vertex/fragment
profile, not modern compute-container compatibility or Metal registration.

Preceding frozen combined native build: `build/macos-product-wire-power-final`,
91 translation units, all 406 kernel imports resolved and all 233 source
files verified. Executable SHA-256 is
`c1cb8e7f402bc77c43682011e074ba2388f5f99cd673cc22f9f53104d1456aec`,
UUID `951E2F2C-F37D-3B85-ABE4-A348BC9998DE`. The 9,724 KiB stage was removed.
This includes the actual v1.3 Session/IOUserClient product boundary and
current-state power consumers. Native OFF-state validation also completed:
11,554,469 capture and 50,298,906 retirement checks, zero failures; three
kernel objects compiled and the 2,532 KiB stage was removed. Real IOKit dylib
and four tools are exported in `build/native-transport-product-v13`; its
155,702 ABI checks passed and its 944 KiB stage was removed.
See [the exact build record](docs/validation-product-wire-power.md).
This artifact predates the later VM reactivation/reusable-resource changes.
Do not describe current mutable sources as its validated inputs. It has not
been loaded or executed on the target and is not a replacement for the
immutable 45eab capture package. Continue the internal consume-job and
two-job validation work from [the design](docs/compute-reusable-resources-design.md).

Preceding combined native build: `build/macos-product-admission-final`, 91
translation units, 406 resolved imports, all 233 source files verified.
Executable SHA-256 is
`8d31eb8847b05eab297426994fafc1d2fa12bfaf01f6e4f3fa5681587ba74751`,
UUID `7CF4B810-AE8E-3B67-ADD6-19506B9ED81F`. The focused 2,384 KiB and full
9,688 KiB mini stages were removed. Native validator, dispatch and display
retirement suites passed 22,717,711 checks, zero failures. See
[validation](docs/validation-product-admission.md). No general product user
boundary was included in that snapshot. The later v1.3 boundary is included
in the c1cb8 build above. Target execution and Metal registration remain
unestablished; preserve this earlier artifact's original scope.

Preceding combined native build: `build/macos-runtime-recovery-final`, 88 translation
units, 406 resolved imports, all 228 source hashes verified. Executable SHA-256
is `5624a26ba6076a9d18cb0f1c75535fc04c3cb53e89f00eabef10a52f6468bb61`,
UUID `2FC2F154-8126-32D7-8C65-649BFF49EA3C`. The 9,568 KiB full-build stage
and 3,864 KiB focused test stage were removed. Native sanitizer and three
fresh host-suite results are in [validation](docs/validation-runtime-recovery.md).
No target load, shader execution or Metal registration is implied.

Preceding combined native build: `build/macos-smu-clock-owner-final`, 88 translation
units, all406 imported symbols resolved and all228 source hashes verified.
The 9,524 KiB stage was removed. Executable SHA-256 is
`b1e33d63e223c271d3580d0fbbc5df1a706f3429973040b4ef497665a6b3a429`,
UUID `ECC2CA6E-F9BA-3EE1-8C20-751015B139E9`.
See [the integration record](docs/validation-smu-clock-owner.md) for ownership,
teardown, sanitizer/native checks, activation limits and exact artifact hashes.
This development build is separate from the frozen target capture package.

Preceding combined native build: `build/macos-compiler-uclk-final`, 86 translation
units and 406 resolved imports. All 225 source hashes matched after export;
the 9,452 KiB stage was removed. Executable SHA-256 is
`932b945bf9264e6f94c2e7b4e509282c54ac5742abd45ee19d98436603b0e857`,
UUID `C480114D-839D-3EDE-86CA-1FC94CC3C858`. Six targeted suites passed.
See [the integration record](docs/validation-compiler-uclk.md) for exact
source/build hashes, focused checks, ownership warnings and runtime limits.

The compiler fixture verifier is mandatory in native/cross/host builds; remote
source snapshots include its pinned artifacts and check them again on mini.
The explicit v1.2 class, three private control pages and real GPU readback are
connected in source, but have never run on target. UCLK owns a retained,
fulfilled minimum only under its restricted Prepared-state admission; no
activation callsite, SOC/Fabric hold or complete modeset is claimed.

The owned dispatch/pipeline compiler product passed final native revalidation
in `build/metal-pipeline-native-20261011b`: all16 commands, all23 exports, source
hashes matched, 135,016,925-byte stage removed. Dylib SHA-256 is
`393c71ea8196473b6c762016a09348b1b27caca85896087d80a819983ea35072`.
See [PIPELINE.md](userspace/metal/PIPELINE.md); no GPU submission occurred.
Those retirement integrations and the client shader-memory fix are now in
`macos-runtime-recovery-final`. The source-derived GFX1100 leaf validator,
canonical multi-region builder and native immutable capture have now been
implemented and independently reviewed; see [the design](docs/compute-product-admission.md).
Build-list integration, focused native validation and the combined 91-unit
link are complete in `macos-product-admission-final`. The general
product's subsequent Session/IOUserClient exposure is linked in the c1cb8
milestone above, but no target execution is established.
Keep later changes distinct from that frozen build's evidence. Remote stages stay bounded
and are removed after export. No target smoke program is run on mini.

Current work beyond the older snapshots below adds the actual native user
client, isolated clear/copy session bridge, normal completed-VM retirement and
board/clock inputs for DML. The 85-unit combined build resolves 406 imports;
all 187 source hashes matched, six targeted suites pass, and its 8,244 KiB
remote stage was removed. Aggregate results have a 64 MiB / 64-holder cap.
Read-only startup preflight and failure-reporting improvements are now reviewed
and included in `build/macos-client-preflight-final`: 85 translation units,
406 resolved imports, 187 source hashes verified, and its 8,276 KiB stage
removed. Executable SHA-256 is
`4f7b7ec5166190d83eb526625b99d363491a6ca717e2c908d9ee540cd82a5ffd`;
UUID is `F2363CC2-E023-3B32-8446-017ADFDC9FD0`. Continue from
[the integration record](docs/validation-client-display-inputs.md), not from
the earlier immutable capture source. No client GPU smoke tool has run on mini
or the target, and the diagnostic USB procedure does not enable this path.

The newer development build `build/macos-audio-console-final` includes 78
sources, native audio stop, VM/GART cleanup retention, Reset PCI validation,
BME/MMHUB ordering and actual EFI-console reservation capture. All 358 imports
resolve and its 7,896 KiB stage was removed. It is not the immutable follow-up
capture package and has not been target-loaded. An earlier combined attempt
at `build/macos-audio-console-integration` had an unavailable metaclass import;
preserve that failure evidence and use the final report for development checks.
Read [cold-boot admission](docs/cold-boot-admission.md) before changing the two
pre-PSP media checks: untouched firmware-first media initialization and recovery
of driver-published DMA need different admission, without fake stopped results.

Newer native-linked source now replaces those Engine-only gates with distinct LSDMA
checks under a consumed FunctionGroup FirstAttach token. Full diagnostic media
checks remain intact. Engine/Lifecycle start accepts a real Operation, retirement
accepts the callback-only RetirementScope, and the worker must complete before
ACK/PCI close. Newer development source constructs/registers those owners only
under the additional `-hackynavi-engines` opt-in. Descendant callbacks require
actual access scopes, and the Device joins retirement before unmap/close. This
path is not in the immutable capture package and has not run on the target.
Focused compilation evidence is in
`build/native-first-attach-scope` and `build/native-callback-retirement-final`;
the earlier combined link is `build/macos-scoped-first-attach/build-report.json`.
All 80 sources compiled, all 364 imports resolved, source hashes matched, and the
8,016 KiB remote stage was removed. Executable SHA-256:
`2556e364efac5701d777d1196f3ddf4b1ccadd7a64503d86407e0633cdf2091c`,
UUID `75261275-5964-3D25-9DDD-EB1372434AC2`. This is not target boot evidence.
The combined build also includes ScanoutSurface and HUBP aperture setup: real
reserved allocation/upload and blanked pipe configuration, with callback
retirement and typed reset release. Fetch remains disabled until the modeset
transaction supplies bandwidth, timing, link and console handoff.

The reviewed Device and descendant guard integration is now linked in
`build/macos-device-engines-final/build-report.json`: 81 translation units,
364 resolved imports, no unresolved symbols, all recorded source hashes match.
Its source archive is 3,810,207 bytes uncompressed and the 8,108 KiB remote stage
was removed. Executable SHA-256:
`c3bcacee8f9285e73f592908437e1d0dbcdef299cf32626d1268090f3b0d4b0a`,
UUID `70DE9C99-D805-3649-AE60-263FAE5C87C1`. No runtime/target load was performed.
The first `macos-device-engines` snapshot precedes the primary PM-on missed-wake
fix; preserve it but use `-final` for current evidence. See
[the validation record](docs/validation-device-engines.md).

The seven-table DAL DPM capture and actual Display/Surface access guards are
included. DPM provenance does not replace measured clocks, trusted DML inputs
or native modeset ownership. The frozen `1e4040229efa` diagnostic package is
still the target handoff; do not add the engine opt-in to that procedure.

## Current Windows workflow

Run from `C:/Users/Ras/Documents/ChatGPT/HackyNavi`:

```powershell
git status --short --branch
git log -4 --oneline
& .tools/build-python/cmake/data/bin/cmake.exe -S . -B build/cmake
& .tools/build-python/cmake/data/bin/cmake.exe --build build/cmake --clean-first -j4
& .tools/build-python/cmake/data/bin/ctest.exe --test-dir build/cmake --output-on-failure
python scripts/cross-check-kext.py
python scripts/build-remote.py --host victor@mini
```

The CMake paths above are this checkout's existing local tools. The remote
build uses existing Xcode, verifies the pinned firmware, sends a small source
archive, requires at least 1 GiB free, and removes its unique staging directory.
It neither installs dependencies nor loads the kext. Preserve the final report
and verify its source hashes; an interrupted run may leave the previous report
in place. Poll an existing process/session instead of starting a duplicate.
After interruption, inspect exact staging paths and live processes before
cleaning only abandoned HackyNavi staging directories.

This host has shown timestamps moving backward: Ninja incorrectly reported
no work after edits. Explicitly regenerate CMake after build-list changes and
force recompilation when timestamps cannot be trusted. Hash source snapshots.

## Repository and optional future transfer

Origin: `git@github.com:imrasalghul/HackyNavi.git`

Working branch: `codex/navi31-bringup`; observed HEAD is now `47fa6b9` after an
external amendment of `b172aac`. This team did not amend/push that history.
If migration is requested again, regenerate the Git bundle for local history
and the working-tree overlay for all current changes and new files.
Neither an old migration package nor this document implies changes were pushed.
Use `git status`, `git log -5 --oneline`, and `git diff` rather than assuming the
remote branch contains the working tree. No SSH keys or credentials are copied.

For a future transfer, generate `MIGRATION-MANIFEST.json` with each payload
file's hash/size and include selected reports/logs under `migration-evidence/`.
Compare report source hashes before attributing results to newly edited files.
Include the manifest-pinned nine firmware files and license from
`.research/firmware`, plus the exact GPU ROM as `.research/rom/Navi 31.rom`;
keep the ROM private. Include only needed local reference files under
`.research`; exclude large upstream checkouts and Windows tool/build caches.
These describe package contents to prepare, not a confirmed Mac import. Fetch missing reference
files at the pinned revisions below, preferably individually or with sparse
checkouts. Do not download entire Mesa/Linux histories merely to read a file.

## If development later moves to the Mac

From the imported repository root:

```sh
git status --short --branch
df -h .
sw_vers
xcode-select -p
xcrun --sdk macosx --show-sdk-version
python3 --version

# Portable C++ tests plus Python tests; no CMake dependency needed.
python3 scripts/build-host.py --compiler "$(xcrun --find clang++)"

# Build an unsigned Intel kext, without installing or loading it.
bash scripts/build-kext.sh "$PWD/build/macos"
python3 scripts/inspect-kext.py "$PWD/build/macos/HackyNavi.kext"
```

If CMake is already available, the alternative is:

```sh
cmake -S . -B build/cmake -DBUILD_TESTING=ON
cmake --build build/cmake -j2
ctest --test-dir build/cmake --output-on-failure
```

Targeted address/undefined-behavior sanitizer checks for the latest clock work:

```sh
mkdir -p build/sanitizers
clang++ -std=c++11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
  -fno-exceptions -fno-rtti -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Iinclude src/discovery.cpp \
  src/display_idle.cpp tests/display_idle_tests.cpp \
  -o build/sanitizers/display-idle
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  build/sanitizers/display-idle

clang++ -std=c++11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
  -fno-exceptions -fno-rtti -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Iinclude src/discovery.cpp src/ip_layout.cpp \
  src/display_smu.cpp tests/display_smu_tests.cpp \
  -o build/sanitizers/display-smu
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  build/sanitizers/display-smu
```

Apple's leak sanitizer is not used. The native build script cleans its unique
temporary staging directory automatically. Prefer one reusable `build/macos`
and `build/host`. Keep at least 1 GiB free as an operational reserve; inspect
`du -sh build .research` before expanding dependencies. Do not remove unrelated
Mac data, shared caches, or Xcode components to make room.

`scripts/build-remote.py` is the current Windows-to-Mac bridge; it becomes
unnecessary for normal work directly on the Mac. The cross-check script is
compile-only Windows checking; native builds remain necessary.

## Validation baseline and limitations

- Fresh clean host build: 50/50 CTest suites passed in 57.97 seconds, including
  the new power-domain, HUBP and pixel-pipeline modules. These model hardware
  I/O, not GPU execution. See STATUS.md for parallel work outside this run.
- Latest kernel-header cross-check compiled 65 x86_64 Mach-O objects, including
  the lifecycle owner and Device revocation/cancellation fixes; included headers
  are not fingerprinted by that report. The corresponding full native build
  completed with 347 imports resolved and its source/header manifest verified.
  Its UUID is D0350C0A-16D1-33AF-B8A2-4A0A77759B35; it has not been loaded.
- The new portable ABI test passed 135,734 checks separately; CMake configures
  51 tests now. The focused run replaced LastTest.log from the prior full run.
- The actual macOS IOKit transport dylib/probe compiled and linked; protocol
  tests passed native ASan/UBSan. See build/native-transport/report.json.
  Kernel user-client handlers and isolated contexts remain to be implemented.
- AIR-to-SPIR-V compiler: nine compiler tests, dynamic C ABI, and two real
  Windows Vulkan 7900 XTX compute tests passed. The latter checked 4,096 and
  4,099 output words with 64 untouched guards each, using the AMD Windows ICD.
  This does not establish macOS Metal or HackyNavi execution. See
  userspace/metal/README.md and build/metal-compiler/test-report.json.
- Frozen native build: Apple Clang 17 / SDK 26.2 on Intel macOS 15.8 (24H22);
  all 343 imported kernel symbols resolved. Includes the earlier diagnostic
  PCI and native clock/idle changes, but not the latest power/HUBP/pixel or
  sleepable lifecycle work. Exact source archive accompanies the test package.
- Prior native ASAN/UBSAN runs passed AVI, VPG, HDMI stream, EDID and DTO suites.
  Latest saved sanitizer report passed display-SMU (5,995 checks) and the
  earlier display-idle (43,608); later power-aware idle and new modules are
  outside that sanitizer run. Its staging directory was removed.
- Native read-only kmutil diagnostics previously showed dependencies OK and
  build-folder ownership errors (user-owned rather than root:wheel). SDK 26.2
  libkmod objects warn about a newer deployment version than the 15.0 baseline.
  Neither this nor successful linking proves the kext loads on macOS 15.8.1.
- The frozen passive diagnostic kext loaded on the actual 15.8.1 / 24H32 target;
  matching binary UUID, GPU identity and BAR configuration were verified from
  supplied logs. See [first boot evidence](docs/validation-2026-10-10-passive-boot.md).
  No engine activation, GPU execution, driver scanout or Metal is verified.

Do not rerun installation or change ownership of system directories merely to
silence build-folder diagnostics. The host's macOS 15.8 build is distinct from
the user's intended 15.8.1 target.

## Hardware and source identity

Target: Ryzen 9 7900X, MSI PRO B650-P WIFI, 32 GiB RAM; RX 7900 XTX PCI
`1002:744c`, revision `c8`, subsystem `148c:2422`.

ROM SHA-256:
`0998f9e7d59f819a7fc6f424f1d97eb65012e886972d3af28d0e79dab27f0f6a`

Legacy ROM image: offset `0x40000`, size 57,856, duplicate at `0x120000`.
Discovery sample `.research/rom/discovery-000b5d00.bin`: 1,756 bytes, 61 IPs.

| Reference | Pinned revision |
| --- | --- |
| Linux AMDGPU/DC | `3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0` |
| Mesa RADV/ACO | `35b085c4c067f198ce0b6437d94678e47b9c645f` |
| libdrm 2.4.120 | `75254bf2390c10644ffb35a90fc8f18f196f9f0c` |
| Cobalt64 | `d6c9aedf8e6526a39e349b5886cbc8c7ddf77f65` |
| NVIDIA macOS reference | `508f3d2496e78a9640d59128a56ddb5a53061606` |
| MacKernelSDK | `3f750085caa17ec3a7880f11c11bf4f48cd6a164` |
| XNU 11417.140.69 | `43a90889846e00bfb5cf1d255cdc0a701a1e05a4` |
| linux-firmware | `afabaf773c4c2e2c841429933a6a084a5af4d14d` |

Full upstream links and licensing context are in `docs/architecture.md`,
`docs/cobalt64-audit.md`, `THIRD-PARTY-NOTICES.md`, and `.research/sources.json`.
Mesa repository: `https://gitlab.freedesktop.org/mesa/mesa.git`.

## Immediate continuation point

The portable display-clock/DAL SMU work was amended externally from `b172aac`
to `47fa6b9`; this agent team did not amend or push it. The older
65-source native snapshot additionally includes all-pipe idle, native DAL/DTO,
display power, HUBP/pixel and sleepable Device/Lifecycle work; it compiled and
linked successfully. FunctionGroup is a separate unintegrated candidate under
review, with real PCI audio ownership and PM observations. Containment after
failed retirement is still required before engine activation.

Parallel lanes now cover native ownership for isolated GPU VM contexts and
Mesa NIR/ACO target-ISA compilation. Genuine Apple AIR ingestion and native
macOS translation/validation now pass; actual ACO code generation passes on
Windows and its macOS backend port is underway. The IOKit client
and compiler are separate userspace artifacts, not kext sources. Preserve
userspace/metal's LGPL source and dependency notices. Keep the frozen passive
release unchanged; its first target boot has passed. Follow PLAN.md's lifecycle
and modeset dependencies before service activation.

`userspace/display` is the real userspace DML32 port, with pinned unmodified
AMD formulas and sanitizer tests. Run `python userspace/display/build-test.py
--sanitize` locally or `python userspace/display/build-native.py --host
victor@mini` for a small native test stage. These calculations do not establish
target clocks or program hardware. Native validation reports include exact
source hashes. Integer encoding now passes 5,526 sanitizer checks locally;
its separate native refresh passed under `build/native-display-encoding` with
Apple Clang 17 / SDK 26.2, verified artifacts and the exact remote stage removed.
All compiled sources match that report; only README changed afterwards.

The shared compile list now has 69 sources including native VM/page-tree
ownership and display bandwidth; CMake configures 54 suites. These counts do
not upgrade the older linked 65-source artifact or prove a fresh full-suite
run. A separate 66-source
snapshot with improved capture diagnostics has now linked natively and been
packaged as `build/releases/HackyNavi-live-capture-6a3a8173c6e2.zip`. Its complete
handoff is inside the extracted package's `HANDOFF.md`; this stage adds the
explicit `-hackynavi-device` argument and limited selector writes. It is not a
Metal test. It excludes the later native VM-owner integration. No USB was edited.

The physical target is Samsung Odyssey G90XF at 3840x2160/165 Hz, currently
HDMI; the user confirms both connectors work under Windows. Read
`docs/monitor-g90xf-4k165.md` and its exact timing/EDID evidence. Legacy TMDS
and uncompressed HBR3 are insufficient for the full-refresh target. FRL/DSC,
pipe planning and runtime validation remain; do not substitute 4K60 fixture
success for completion. HDR remains deferred.

The first passive package remains immutable. To reproduce a future live-capture
release after a fresh build, use `scripts/package-live-capture.py --native-dir
<exact-native-output>`. It validates source/artifact hashes and the executable's
UUID before creating a new immutable directory and ZIP. Analyze the returned
target logs with `scripts/analyze-macos-diagnostics.py --mode live-capture`,
an explicit `--bundle` from that frozen package and an `--output` outside the
original log folder. Never infer acceleration from diagnostic results.

For the DCN3.2 clock work, pinned Linux `dcn32_dccg.c` selects
`dccg2_update_dpp_dto`, not the DCN3.1 variant. Modulo is 255, phase rounds upward;
OTG K1 supports divisors 1/2 and K2 supports 1/2/4. DMU base index 1 contains
DCCG registers; base index 2 contains OTG/HUBP. Legacy HDMI TMDS must not be
confused with HPO/FRL DTBCLK routing. Research exact SMU/global clock and DCN3.2
bandwidth dependencies before enabling the scanout pipeline.

DCN3.2 HUBP0..3 uses power DOMAIN0..3. Resolve the generation's DPP power
behavior from its own hardware-sequencer table; do not import legacy odd-domain
numbering. Keep autonomous DMCUB/firmware actors excluded across the complete
hardware transaction; the native mutex alone does not establish that exclusion.
