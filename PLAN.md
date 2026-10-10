# HackyNavi development plan

Latest integration: [combined graphics preparation](docs/validation-2026-10-11-combined-graphics-evidence.md).
Full native core/aux and updated Metal/tool builds pass; early recovery, paired
function and partial display/DMA evidence are now retained. The next target
test remains withheld until inherited-framebuffer ownership is implemented.

Updated: 2026-10-11. Read [STATUS.md](STATUS.md) for current work and validation,
then [HANDOFF.md](HANDOFF.md), [architecture](docs/architecture.md), and
[Cobalt64 audit](docs/cobalt64-audit.md).

Current working location: the user deferred migration and asked to continue on
the Windows checkout. Keep `victor@mini` as the native build host for now.

## Current parallel integration

User test constraint (2026-10-11): no further probe-only reboot. Before issuing
another target package, audit the complete first-graphics dependency chain and
include all necessary preflight, startup, failure and Metal/readback evidence in
that same test. The next requested kext boot must attempt real graphics work;
do not relabel a capture package as a graphics test. Keep genuine ownership and
failure checks intact. Review the entire dependency audit before packaging.

The supplied `live-3fa3f6d4723b-20261011-163144-730` capture now passes exact
binary UUID and live-capture checks. All 32 context registers completed two
unchanged passes. Pipe0 timing is active; timing1–3 is stopped and those HUBP
functional clocks are disabled. This resolves the timing inventory question,
not the inherited pipe0 TLB0 address contract or dormant-HUBP retirement.
The retained capture and manifest are under `build/target-evidence/` and the
source-backed interpretation is `build/target-3fa-display-review-1/report.md`.

Native typed PSP ownership and Engine retirement wiring are implemented; four
owner units compile against real kernel headers and strict/UBSan protocol fault
checks pass. The portable DP stream/MSA lifecycle is implemented and integrated
into the native/portable build lists; its focused CMake test passes. Neither
change has executed on the target. Physical DP modeset ownership is still open.

The combined engine-mode preflight, source-backed CP/SDMA inventory and bounded
Metal loader/ABI reporting now compile in full native builds. Core/aux build:
`build/macos-combined-graphics-preflight-1`; frontend/four public tools:
`build/graphics-admission-frontend-native-1`. Both remote stages were removed.
No target execution or release follows from those builds.

Recovery-preparation, paired-function and complete/partial BootDisplay/cold-DMA
read evidence are implemented and built. Current parallel follow-up reviews these
changes and investigates the actual firmware framebuffer address/ownership path
from source and the supplied ROM, without another target probe. Known TLB0/dormant-HUBP guards remain unchanged.
Root owns shared build lists, the combined graphics runner/collector, integration
and the serialized native build lane. The complete dependency audit is
[next graphics test audit](docs/next-graphics-test-audit.md). Do not add an OTG enable API accepting a caller's readiness flag:
its actual retained calculation, clock and scanout owners must supply authority.
The supplied 3fa capture package remains immutable while this work proceeds.

The accelerator-port route and real v1.7 vertex-buffer submission stack are
implemented and independently reviewed. Actual native transport, Darwin
compiler/runtime and 22-unit Metal frontend validation passed. The public Metal
geometry-readback tool is compiled, not executed. The compiler stage reused
pinned archives within the 256MiB cap; all remote stages have been removed.
Typed PSP HDCP/DTM protocols and native controller invocation/retirement wiring
are implemented; a new full native link including the latest wiring is pending.

The bounded read-only OTG/VTG, MALL and VM-context inventory and its guarded
native capture/publication are implemented and independently reviewed. The
112-unit core and two-unit late accelerator compile; the new core-only
capture package is `build/releases/display-context-20261011/HackyNavi-live-capture-3fa3f6d4723b.zip`.
It adds 32 registers with two-pass validity/change masks. Preserve the tested
`9eda6a40f88c` package as rollback. The reservation gate remains closed for
the target's unexplained TLB0 and powered/unblanked zero-viewport pipes.

Next work proceeds in parallel: investigate source-supported TLB0 semantics,
audit all first-graphics prerequisites against the supplied capture, and connect typed PSP protocols
to native controller ownership. Only after the display-memory reservation and
remaining startup predicates are satisfied, perform the first engine boot,
late accelerator discovery and bounded Metal readback. Root owns the serial
native build lane and packaging. None of the completed build work establishes
target engine execution, display takeover or WindowServer acceleration.

The current priority is resolved in source and native builds: c39 failed
OpenCore injection because the early driver imported the SystemKC IOAccelerator
superclass. The early core and two-unit genuine late accelerator are
now separate. Independent review fixed a publication/retirement cleanup race;
all 412 core imports are measured BootKC providers. The preceding boot-test package
was `build/releases/loader-fix-20261011/HackyNavi-live-capture-9eda6a40f88c.zip`.
The replacement load/capture check has now passed on the actual target with
the exact binary UUID and both services active. Its complete raw display
snapshot fails the inherited address decoder, so interpret the observed
AGP/TLB/flip/in-use fields against pinned DCN3.2 sources and establish the full
scanout reservation before an engine test. Late accelerator deployment and
first real engine/Metal tests remain outstanding.

PSP trusted-application native ownership is implemented and included in this
build, with strict/UBSan, kernel-header and CMake checks passing. It does not
adopt inherited HDCP sessions. The later native typed-command wiring is now
implemented but has not executed on the target. The
Cobalt64 follow-up identifies an accelerator-port native-client route and
IOSurface presentation patterns for subsequent work; its fixed modes and
borrowed Apple compiler profile are not adopted.

The following earlier graphics milestones remain implemented in the core:

The reviewed v1.5 native graphics path is enabled in source. The Metal render
pipeline, command encoder and ordered texture synchronization are wired, with
independent ownership/descriptor reviews complete. The corrected Metal viewport
passed a new 107-unit native kext build. All 21 frontend units now compile and
link against the real native SDK; real compiler/layout validation also passes.
The graphics package is built and independently verified. Native inherited HDMI
backend observation is now integrated after the actual inherited DMCUB stop;
the full 109-unit kernel builds and all 414 imports resolve. This observation
does not grant display takeover or clock authority. The independently reviewed
OPP solid-black and separate MPC/OTG stop transactions are implemented and
locally validated, including the actual DCN32 optional-callback differences.
Neither grants native display takeover. Inherited AVMUTE with three observed
frame cycles and actual AFMT sample muting is now implemented and independently
reviewed, without a native mutating callsite. Typed PSP HDCP/DTM application
protocols and inherited protection state remain prerequisites for that callsite.
Actual Apple fragment-buffer AIR now has a real compiler, independent kernel
instruction admission, derived hardware descriptor, retained native buffer
capture, v1.6 IOKit transport and Context draw path. The full updated kext builds
and its 271 source inputs, 109 units and 414 imports are independently verified.
Real Metal buffer bindings and a target-only smoke are now connected and
independently reviewed. The updated 22-unit frontend and three target tools
compile/link on the Mac. The actual compiler-backed runtime passes 54 native
ASan/UBSan checks over four pipelines. The development package is built and
checks 130 files, 4,343 source members and 13 native images. First-engine
readiness and frontend artifact audits are complete as source/build evidence;
actual target execution is outstanding.
Root owns shared build lists and the serialized, bounded `victor@mini` lane;
no GPU or Metal object runs there.

The user's previous failed test package is:
`build/releases/current-20261011/HackyNavi-live-capture-c39afcee43f9.zip`.
That boot failed before driver startup: the USB OpenCore log confirms its hard
IOGraphicsFamily subclass dependency cannot be prelinked from BootKC. Current
correction separates the early core from the genuine late accelerator and
prevents SystemKC-only imports in early packages. Do not repeat unchanged c39.
See [loader evidence](docs/validation-2026-10-11-c39-loader-failure.md) and
[Cobalt64 findings](docs/cobalt64-acceleration-followup.md).
Full HDMI takeover is not
an offscreen-test prerequisite; see
[first engine test readiness](docs/first-engine-metal-target-readiness.md).
PSP TA ownership and vertex-buffer kernel support are in the new 112-unit build.
The boot-capture package keeps engines disabled and contains no Metal bundle.

The first draw profiles are one triangle, Position-only NGG from generated
positions or one retained 48-byte vertex-buffer window, constant RGBA or one
actual 16-byte fragment-buffer input, and one linear RGBA8 target. The buffer
Metal frontend is native-linked but not target-tested. This is a foundation for the Metal
render path; it does not satisfy general rendering or desktop acceleration.
The native C++ compiler/runtime integration checks and dependency packaging now
pass. Next complete display/presentation and startup/recovery paths, while
implementing the remaining Metal compiler/resource contracts. The target-only
render smoke checks actual readback,
color, exterior preservation and Y orientation; it has not run on hardware.
Continue rejecting unsupported operations explicitly.

Apple runtime MSL compilation still needs a truthful `featureProfile` and
compiler-service contract. Actual macOS method inspection confirms that an
explicit language version does not bypass that requirement. Do not invent a
profile or enable an internal Apple environment override. Existing genuine
precompiled metallib translation remains the implemented compiler route.

Independent review found and fixed eight GFX10-only color-register writes in
the first draw stream. The replacement is 712 DWORDs with complete register
address coverage and real Mesa dynamic color-state comparisons. Preserve
`build/gfx11-graphics-draw-local-2` as superseded failure evidence; local-4 is the
reviewed corrected register implementation. A subsequent integration check found
that genuine Apple vertex SPIR-V preserves clip-space Y, so Metal requires a
negative hardware Y scale. Preserve the positive-Y builds as historical evidence;
the correction now passes its own oracle and native build. See STATUS.

## Objective and current truth

Implement a real RX 7900 XTX / Navi 31 driver for Intel macOS 15.8.1, including
GPU initialization, command submission, rendering, Metal acceleration, and HDMI
and DisplayPort output. The user explicitly rejects production mocks, dummy
implementations, or success reports based only on compilation. Host register
models are tests, never evidence of GPU execution.

This is NOT a working accelerated driver yet. The frozen passive kext has now
loaded on the actual macOS 15.8.1 target and captured the matching GPU's PCI
identity/BAR configuration. See [first boot evidence](docs/validation-2026-10-10-passive-boot.md).
No HackyNavi shader execution, driver-controlled display output or Metal device
has been demonstrated. Translated AIR has executed correctly on the target GPU
through the installed Windows Vulkan driver; that verifies compiler semantics
only. The delivered PCI test remains diagnostic. Development source adds the
separate `-hackynavi-engines` opt-in to invoke the real startup coordinator,
paired-function ownership, scoped hardware access and callback retirement.
That path is native-linked and reviewed but has never run on the target.

OpenCore, installation media, and macOS installation are handled in another
session. Keep this project focused on the driver. The Mac mini is a development
host, not evidence that the target AMD GPU is present. Do not load the kext there
as part of routine builds.

The user now reports macOS installed and OpenCore on USB. Prepare a concrete,
traceable driver build and test procedure before requesting target testing.
The supplied logs now establish both passive loading and the later explicit
live-capture boot. The user reported HDMI and no incident during that capture;
this does not establish engine execution or accelerated display output.

Latest user priority: make the driver bootable with real Metal; **HDR is
explicitly deferred**. The user has now requested a diagnostic kext and an
OpenCore-agent handoff. The frozen passive test package and exact procedure
are described in [OPENCORE-DIAGNOSTIC-HANDOFF.md](docs/OPENCORE-DIAGNOSTIC-HANDOFF.md).
That first test provides loading/PCI evidence, not accelerated-driver readiness.

Display target clarified: Samsung Odyssey G90XF, 3840x2160 at 165 Hz. The user
reports both HDMI and DisplayPort work under Windows, with HDMI connected now.
Read-only Windows capture confirms the active HDMI timing, including a
1,629,250,000 Hz pixel clock and 4224x2338 totals. Do not treat the existing
4K60 fixtures as verification of this mode. HDMI FRL and DP DSC/high-refresh
planning are explicit remaining requirements; HDR stays deferred. Capabilities
from an HDMI EDID do not establish a DP receiver's DPCD/DSC state.

## Existing implementation to preserve

Current parallel integration work:

- Completed package: `build/metal-bundle-package-final-3`, five unchanged real
  native binaries with compiler/helpers/licenses/source. Root verified 88 files
  and 4,198 source members. Compiled Objective-C signatures match all 391 own
  required methods; genuine inherited notification hook remains. No install or
  discovery/GPU test has run.
- Completed coordinated controls: independent review, 306,452 native sanitizer
  checks and eight native kernel objects; all 88 inputs verified. Divider/PLL
  observations remain distinct from measured clocks and fixed-clock ownership.
- Completed Engine/Display retirement: actual consumer stop, queue/VM/RLC
  retirement, clock-policy retirement and only then DMCUB stop. Independent
  reviews passed; native validation checked 71,556 sanitizer assertions, nine
  kernel objects and 105 archived inputs. Reset-only policies remain retained.
- Completed graphics barrier: matches actual pinned Mesa's CB/DB timestamp/PWS
  sequence. The queue's existing final fence already flushes the full pipeline.
- Completed local cold-clock owner: native same-generation journals, actual
  PLL/DID observations, DTO/surface admission and portable bounded two-step
  transition. Independent reviews and the combined native Mac check passed.
- Completed Apple graphics fixture: actual AIR/metallib, four identical-pair
  compiler products, 36 checks and independent review; no GPU execution.
- Completed Mesa color layout: 493 actual products per run, independent review
  and normal build registration. Numerical layouts confer no mapping authority.
- Completed local graphics ISA admission: copied resource-free NGG/PS images,
  exact instruction/ABI validation, 40 real product pairs, 97,170 strict and
  UBSan checks each. Build registration, CMake and independent review pass.
- Completed power guard correction: retain complete current-ON status/config
  pairs throughout scanout and reject changes; independent review passed.
- Root: combined native validation completed (eight suites/41,992,134 checks,
  19 kernel objects/119 inputs). Integrate and review real kernel resource
  capture, graphics submission and cold display mode ownership next.
- Native resource worker: same-owner PCI/GC/NBIO topology capture, then exact
  BO handle/mapping/range/color leases transferred into retained queue jobs.
- Draw worker: source-derived GFX11 preamble, real topology and owned GE
  attribute-ring requirements, then fixed offscreen triangle command encoding.
- Recovery worker: actual never-started Prepared queue retirement; broader
  firmware-loaded/no-CSIB RLC retirement remains a separate sourced protocol.

- Full frontend milestone: `build/metal-device-native-2` now compiles all 14
  units and links both the component dylib and actual Metal bundle; 31 native
  commands pass. Root verified 40 inputs, all objects and four exports. Next,
  static compiled-method inspection and packaging are completed as above.
  Bind the target-only smoke to that package when target startup is established.
  This is still compute integration, not a completed renderer or target GPU test.
- Full kernel integration passed in `build/macos-graphics-clock-integration-1`:
  102 units, 412 imports, 252 verified inputs. New clock ownership, topology
  capture, shader/color validation, power guards and ordered retirement are
  included in one complete kext. No engine-enabled target boot has been tested;
  the latest returned logs used only `-hackynavi-device`. The next already-issued
  capture is still needed to observe the current boot-display state.

- Root: connect actual Managed CPU/GPU storage through Metal uploads and
  ordered readbacks; freeze and compile the combined object layer and a
  public-Metal target smoke. The test must discover the real device and execute
  A-to-B-to-C with guard verification; building it is not executing it.
- Display worker: genuine `_MTLDevice` frontend, owned compiler paths,
  mixed compute/readback commands and public buffer factories.
- Capability worker: implement limits and queries from measured macOS ABI
  and actual backend limits; do not claim an unsupported Metal feature tier.
- Compiler worker: add explicit immutable tool paths to AIR translation and
  subsequent pipeline validation; a plugin must not alter application-global
  environment variables or depend on developer tools in PATH.
  Completed native validation: 94 explicit C ABI plus 27 C++ ownership checks,
  32 exports; root verified the 44-input archive and exported dylib. Next,
  package those verified helpers with the reviewed frontend and verify linkage.
- Completed this step: exact SOC/Fabric deep-sleep inhibition/restoration,
  138,600 native sanitizer checks and four kernel objects. Integrate the
  reviewed policy with the display clock owner. Root added explicit owned DF
  DISALLOW/terminal ALLOW policy, passing independent review, 153,589 native
  sanitizer checks and four kernel compiles. No prior-allowance restoration is
  claimed; connect these real policy owners into the complete clock transaction.

- Root: FAMS/DAL coordination passed 29,370 native sanitizer checks and
  three kernel compiles. Keep its reset-required partial-policy retention;
  integrate it with actual fixed clock ownership before allowing a modeset.
  Its bridge into live-DMCUB UCLK requests now passed independent review and
  native compilation. The public IOAccelerator child is integrated with the
  existing lifecycle/paired retirement and the complete 94-unit kext is built;
  the latest full build includes the UCLK bridge. Continue fixed-clock policy,
  recapture/calculation ownership and real display activation.
- Metal command objects: implemented and natively linked. The focused run
  passed 55,881 sanitizer checks; independent reviews cover blocking capacity,
  slot release before callbacks and scheduled-handler waiting. No actual
  Metal command object has executed yet. Workers now implement HNMetalDevice
  using the measured base hooks and investigate the actual bundle trust/path
  policy. Root integrates the reviewed IOAccelerator child and full kext link.

- `HNMetalLibrary` and compute `HNMetalFunction` now compile/link natively,
  backed by owned metallib AIR and a retained actual Shader. The focused
  `native-compute-shader-library-final-2` run passed252 ASan/UBSan checks;
  the owned compiler ABI passed460 native checks with31 exports. Both stages
  are removed. Register-aware kernel launch admission is now integrated, with
  471,170 focused native sanitizer checks and a successful changed-object
  kernel compile. Continue with real Metal pipeline/command objects and
  measured accelerator integration. The `macos-compute-admission-final` full
  kext includes this occupancy change; no target execution is demonstrated.
  Source compilation as a driver API remains open. The already-installed
  Metal Toolchain works when selected explicitly; a real macOS15/MSL2.4
  compute AIR and MTLB2.8 library were compiled offline and exported. The new
  bounded parser passed its native refresh: 460 existing and 48 new compute
  ABI checks, 31 exports, verified artifacts and removed staging. No download
  or installation was needed.
- Native ISA admission now supports the actual one-thread uniform products
  and packed local-ID operation for multidimensional groups. Independent
  review, 220,693 focused native sanitizer checks and the changed kernel
  object compile pass. The actual Shader/compiler/admission boundary passes
  265 native ASan/UBSan checks across nine cases/twenty variants.
  `HNMetalComputePipeline` is independently reviewed and native-linked in
  `build/native-compute-pipeline-final`; seven earlier objects are byte-identical,
  all six exports are verified and staging is removed. Actual command queues,
  command buffers and compute encoders are now being implemented in parallel
  with their production scheduling state. Discovery work measures the public
  IOAccelerator/own-client route before introducing private family inheritance;
  see [the measured boundary](docs/metal-public-accelerator-route.md).
- Latest evidence supersedes the FORCEON hypothesis below: the now-loaded
  `dbbcb5ec0ba9` capture reports stable `0x10000000`/CONFIG0 on all four domains.
  The corrected capture and bounded VCN POWER_STATUS inventory passed focused
  native validation; the selected five-file capture build is frozen as
  `HackyNavi-live-capture-45eab551a719`, with 74 units, 353 imports and both
  mini stages removed.
  [Target evidence](docs/validation-2026-10-11-config0-capture.md). Preserve
  old immutable packages; use the new package's own handoff for target testing.
- Native product v1.3 wire, actual IOKit client and Session/IOUserClient
  execution bridge are implemented with copied code and real completion.
  155,702 protocol checks, real kernel objects and independent review passed.
  Independent smoke-tool review and native IOKit compilation/linking passed;
  the dylib and four tools were exported and the 944 KiB stage removed.
  Six native kernel objects and all three native power suites now pass;
  fresh CMake passes all four selected suites. The focused stage is removed.
  The full `macos-product-wire-power-final` build has 91 translation units,
  406 resolved imports and 233 verified source files. Its immutable executable
  is `c1cb8e7f402bc77c43682011e074ba2388f5f99cd673cc22f9f53104d1456aec`;
  the 9,724 KiB stage was removed. No target load, GPU execution or Metal claim.
  [Current integration evidence](docs/validation-product-wire-power.md).
- Current-FSM interpretation is reconciled throughout `cold_dma`,
  `display_idle` and `display_power`, including adoption, idle checks, polling
  and final stability. CONFIG checks, full raw stability, ownership, ambiguity
  retention and DMA-idle requirements remain. Independent review and native
  suites passed. The later boot-display OFF-state correction also passed
  11,554,469 native capture and 50,298,906 retirement checks; its 2,532 KiB
  stage was removed. Both corrections are in the full c1cb8 build, separately
  from the immutable 45eab capture package.
- Bounded `.metallib` extraction and compiler integration passed native
  validation in `metal-metallib/native-run-2`: 17 commands, seven actual SDL
  functions, 25 negative C ABI cases, 23 exports and removal of the
  158,219,947-byte stage. Native CLI was explicitly skipped; separate local
  CLI and 15 Rust library tests passed. Keep the verified MTLB 2.5 profile
  distinct from unverified modern compute containers. Real Metal library/
  function objects and device discovery remain implementation work.
- Reusable native resource/job ownership is implemented and independently
  reviewed: exact completion, both-hub detach, private-control retirement,
  preserved data BO generations/root/VMID and checked reactivation. Focused
  native validation passed eight objects, 241,988 VM-context and 5,202 queue
  checks. The immutable `macos-reuse-final` build has 92 units, 406 imports,
  234 verified sources and executable
  `fe01c5be0f8838dd238fd079cf7709324a81a69d13254e9590f5c50ec854f66b`;
  both the 3,820 KiB and 9,792 KiB stages were removed. Its internal A->B->C
  probe is connected after ShaderVerify but has not run on the target.
- The explicit v1.4 Session/client profile, eight bounded historical results
  and release semantics are implemented and independently reviewed, with
  older profiles preserved. Strict and UBSan protocol tests each passed
  158,417 checks; three real kernel-header objects compile. This source is
  outside the frozen fe01c5be archive. The supplied-source `--reuse` smoke
  cross-compiles and passes actual-client compatibility review. Native
  transport validation linked five artifacts, passed 158,417 ASan/UBSan checks
  and removed its 1,040 KiB stage. The combined v1.4 build is now frozen at
  `build/macos-reusable-v14-final`: 92 units, 406 resolved imports, 234 sources,
  executable `67fc0db187d72a5fd459194924d0f17b91dd6f20c3a08c69eee6d19863ea8cb6`.
  The 9,808 KiB full and 616 KiB focused native stages were removed.
  Actual target execution remains. Preserve the
  [native lifetime contract](docs/compute-reusable-resources-design.md).
- Validate the real synchronous `compute::Context` backend and immutable
  compiler-product snapshots, then its target-only
  [A->B->retained B->C test](userspace/runtime/CONTEXT-SMOKE.md). It uses actual
  persistent native BOs, explicit copied I/O and stable GPU addresses, and
  closes on uncertain failures. Native runtime compilation and 233 actual
  compiler-backed ASan/UBSan checks pass; the target tool links without being
  executed. The private `HNMetalBuffer` passes strict ARC compilation, and its
  later exact-context identity correction passed a focused ObjC relink.
  Device discovery, command objects and target execution remain separate work.
- Connect actual library/function objects to the reviewed, native-validated
  owned metallib C ABI's immutable names, metadata and AIR copies. Local
  evidence covers 19 Rust tests, 460 DLL ABI checks, all seven real SDL AIR
  functions compiled after library free and unchanged compiler/CLI behavior.
  Native validation passed 18 commands, the same 460 checks and exactly 31
  verified public exports; its stage was removed. The parser's measured MTLB
  2.5 format remains unchanged; no modern compute container support is inferred.

Earlier completed snapshots and their original scope:

- New target evidence: `1e4040229efa` loaded with matching UUID, all twelve
  collector commands passed, and all fourteen source files were preserved.
  DOMAIN0 raw status is `0x10000000`; CONFIG and confirmation are missing
  because the tested predicate stopped first. The source-backed correction
  now requires fresh CONFIG1 and stable confirmation for that status. The
  new four-pipe power inventory and raw display reporting are native-built
  in the frozen `dbbcb5ec0ba9` capture package. Native sanitizer validation
  passed 5,033,482 assertions and all 353 kernel imports resolved. Mini stages
  are removed; the next step is a target boot and fresh collection using its
  packaged instructions, without engine activation. See
  [power capture](docs/validation-2026-10-11-power-capture.md).
- The FORCEON audit found later development predicates in `cold_dma`,
  `display_idle` and `display_power` still have narrower admission. They fail
  closed; the diagnostic capture correction does not complete engine startup.
  Reconcile those actual owner/protocol checks with the source-backed power
  contract before preparing an engine test. `boot_display_stop` already checks
  the retained raw power/config values exactly; its forced-on regression
  passed 22,512,972 local sanitizer assertions. Do not change the narrow
  capture artifact's scope.
- The [VCN all-ones audit](docs/vcn-allones-audit.md) confirms the captured
  PGFSM address and shows Linux reads POWER_STATUS before attempting its
  broader register dump. Plan a separate bounded POWER_STATUS inventory with
  validity tracking; it is not in the frozen display-capture package. A tile
  access indication is not completed power-off or stopped DMA, and the
  diagnostic failure is not established as the engine path's sole blocker.
- The real ISA validator, canonical multi-region PM4/descriptor/push builder
  and native immutable product capture are implemented and reviewed. Actual
  copied shader bytes determine resource access and RSRC; no supplied report
  or provenance assertion grants admission. Local checks pass 84,007 decoder,
  120,732 builder and 822,327 native interval-helper cases. The parallel
  build-list integration, native sanitizer checks and full 91-unit link are
  complete: all 406 imports resolve and all 233 source files match; both mini
  stages are removed. See [native evidence](docs/validation-product-admission.md).
  The later bounded product transport through Session/IOUserClient is now
  implemented as recorded above. That boundary was not part of this prior
  snapshot. No general product GPU execution or Metal device is established.

- Completed and native-linked: PPSMC SOC/Fabric companion plus exact table5
  metrics through a private native VRAM page. Actual scope, common firmware
  fault state and deadlines span all accesses. Engine teardown closes completed
  tables and returns touched ranges to automatic policy only with fresh
  quiescence; ambiguity retains resources. 88 translation units, all406 imports,
  all228 source hashes verified; 61,081 sanitizer checks also passed natively.
  [Validation](docs/validation-smu-clock-owner.md). Normal request/capture
  activation remains gated on complete display and recovery contracts.

- Completed and native-linked: exact Apple AIR-to-ACO shader class through
  native VM/queue, Session/IOUserClient and explicit v1.2 copied-I/O transport.
  Private code/descriptors/commands, 4,096 words plus 64 guards, actual GPU
  fence/readback/retirement and target-only smoke tool are connected. The
  combined 86-unit build resolves 406 imports; all 225 source hashes matched,
  six targeted suites passed, and its 9,452 KiB mini stage was removed.
  [Evidence and runtime limits](docs/validation-compiler-uclk.md).
- Completed in the same build: real DAL UCLK minimum with `0x24` fulfillment,
  raw reply and failure journal, restricted Prepared native entry and scoped
  boot. No activation callsite exists yet. Normal stop retains a possible
  UCLK constraint for verified reset; unsupported zero withdrawal is rejected.
- Completed offline: owned dispatch/pipeline products invoke real NIR/ACO for
  each local-size variant, retain code/RSRC/reflection/resource accesses, and
  discard all variants on any error. Two real kernels at local64/local3 pass
  Windows/native C ABI checks, all23 exports are linked, seven Rust tests pass,
  and native deliberate-tail failure proves no partial publication. The final
  native stage was removed. [Contract/evidence](userspace/metal/PIPELINE.md).
- Completed and native-linked: terminal SMU table reconciliation before the
  first MODE1, exact-Operation native-client runtime-failure retirement, and
  VMID1..7 shader-memory configuration. Full88-unit link resolves406 imports,
  all228 source hashes match, native sanitizer cases pass127,358 +17,403,
  three freshly built host suites pass, and remote stages are removed.
  [Validation](docs/validation-runtime-recovery.md). Failure handling remains
  terminal; no seamless restart or general Metal execution is claimed.
- Active next work: finish native validation of the bounded product boundary
  and actual compiler-to-IOKit smoke path, then reusable resources, actual
  Metal objects and discovery. Follow [the concrete design](docs/compute-product-admission.md);
  compiler JSON or a hash list never grants ISA admission. FAMS, trustworthy
  DML consumer, modeset and IOSurface remain.

- Completed and native-linked: actual task-bound IOUserClient and one-job clear/copy
  sessions, with real VM allocation/mapping, copied uploads, GPU completion,
  result readback and normal retirement. The older generic profile remains
  unsupported; this is not Metal registration. Aggregate retained results now
  have a 64 MiB / 64-holder bound after VMID reuse. The combined 85-unit build
  resolves all 406 imports; six targeted integration suites pass. See
  [evidence](docs/validation-client-display-inputs.md).
- Completed in source and CPU/native checks: ROM topology/reference capture plus twice-read DAL DPM
  tables/DC ceilings, and a userspace adapter to actual DML32. Next display
  work is the authenticated producer/consumer and native modeset transaction,
  including clock holds, recapture, bandwidth/watermarks, timing and link state.
  The [clock-owner source audit](docs/display-clock-hold-design.md) identifies
  DAL UCLK fulfillment plus a separate PPSMC SOC/Fabric/metrics owner, DCF
  deep-sleep and DMCUB coordination. UCLK reply units and the existing zero-MHz
  withdrawal claim remain unresolved; neither may be treated as proven policy.
- Completed and reviewed after activation audit: negative display/console preflight before
  audio/DMCUB control writes and exact raw failure propagation. A positive
  preflight grants no authority; the mandatory owned capture stays in place.
  The immutable capture has returned DOMAIN0 `0x10000000`; the unobserved
  config/confirmation and control semantics remain under source audit.
  The reviewed patch is in `build/macos-client-preflight-final`: all 85 units
  compile, 406 imports resolve, all 187 source hashes were verified, and the
  8,276 KiB remote stage was removed. No target runtime test is claimed.
- Implemented compute bridge: [execute the exact compiler-produced Apple add-one
  artifact](docs/compute-compiler-integration.md) through a distinct sealed
  native shader class with private descriptors/dispatch payload and actual
  readback/retirement. Keep this separate from general shader upload and
  MTLDevice registration; the current clear/copy capability stays explicit.

- Completed in source: one-use first-attach token bound to the actual paired
  function Operation, distinct LSDMA-only observation, actual sibling HDA stop,
  and scope-required Lifecycle/Engine startup and retirement. No media success
  is fabricated and no recovery path can reuse first-attach admission.
- Completed in source: callback retirement worker, shared terminal transition,
  measured deadline, explicit fail-stop and atomic termination/MODE1 exclusion.
  Device registration/callback wiring and mandatory descendant access guards
  are now integrated in source. Primary teardown revokes/drains even a racing
  registration; failed activation joins retirement on the same kernel worker.
  The preceding 81-unit native link resolved 364 imports; source hashes matched
  at that snapshot, before the current client/model-input integration.
  Target validation remains separate, not a consequence of compilation.
- Completed and native-linked: actual reserved VRAM surface ownership, guarded uploads/readback,
  direct-VRAM HUBP aperture setup and blanked pipe programming. Fetch/modeset
  activation still requires native bandwidth, timing and link ownership.
- Completed: native assigned-BAR convenience calls and unbounded capability
  walks replaced with retained descriptor lookup and bounded read-only PCI
  validation. Combined 80-source native build resolves all 364 imports; it was
  not target-loaded. The frozen capture test package remains separate.
- Completed in source and focused tests: seven firmware-reported DAL clock
  tables, read twice under a real monotonic deadline and actual normal scope.
  A private retained observation binds device generation and capture serial.
  It is not a measured frequency, board DML bounding box or native bandwidth
  authorization. Add the trusted DML producer/consumer and modeset transaction.

- Portable ROM/ATOM and IP discovery parsing, board binding, memory layout,
  VRAM allocation/reservations, guarded transfers, GART tables, HDP flushing,
  VM invalidation, and bounded bootstrap engine/display quiescence.
- Real IOKit DMA allocation and GART storage/controller. The fault-page address
  is backed by an allocation; the term “dummy page” in upstream AMD terminology
  must not be mistaken for a fake allocation or placeholder implementation.
- PSP firmware/ring protocol, SMU wake, RLC/GC initialization, SDMA and graphics
  queues, and kernel engine coordination. Failures retain potentially live DMA
  resources until verified hardware retirement.
- GFX11 clear/copy machine code from pinned AMD/libdrm sources, descriptors,
  dispatch and cache/fence handling. Startup verification compares 10,240 output
  and guard words. This verification exists in code but has never run on the GPU.
- Mode-1 reset, PCI restoration, ATOM ASICInit, and typed retirement proof.
  Proof issuance includes fresh hardware checks, not elapsed time or a flag.
- DMCUB image loading and startup protocol, native serialized display owner,
  AUX/DDC access, EDID parsing/acquisition, DP link training, and HDMI TMDS/SCDC.
- DCN3.2 OTG/VTG/ODM timing, VPG packet memory, AVI construction, HDMI packet
  scheduling/FIFO reset/frame progress. Native HDMI stream ownership binds
  frontend/backend/pipe to the saved link route and retires stream before PHY.
- Latest addition: `display_clock.hpp/.cpp`, per-pipe DPP DTO and OTG pixel
  dividers with discovery-derived addresses, rounded phase, masked shared
  register updates, drained-pipe admission, and restoration after failures.
  It is compiled into the kext but not connected to a complete modeset owner.
- `display_smu.hpp/.cpp` now implements the separate DAL mailbox and requests
  display clock minima, requiring firmware-reported completion for the selected
  clock and DPREFCLK. Requests cannot lower an existing minimum. This portable
  controller is now owned by the native display controller, with fresh
  `display_idle` hardware checks, saved DPP reference frequency, and ordered
  terminal release. Complete transition policy and hardware validation remain;
  it does not calculate bandwidth or program watermarks.

See the implementation and architecture document for exact supported modes,
ownership contracts, and unsupported paths. Implemented protocol != verified
silicon behavior.

## Parallel work and integration order

Work proceeds in parallel; the numbered sections below express integration
dependencies, not a requirement to finish one implementation lane before
starting another. Current ownership and handoff points are in STATUS.md.

- Transport worker (`submission_review`): immutable isolated clear/copy
  submission and readback tokens tied to actual GPU fence plus RPTR are now
  implemented and in all builds. Native ASan/UBSan and six real kernel objects
  pass. Strong VM-registry ownership is now connected to the Engine's typed
  reset transfer. DMA cleanup failures remain latched through empty registries,
  failed GART construction and repeated retirement. Reset now uses bounded raw
  PM/PCIe capability scans; missing PM information cannot imply D0. Focused
  native compilation passes. No exposed page tree or BO is freed on an ACK alone.
- Compiler worker: actual AIR-to-SPIR-V adapter/validator, C ABI and hardware
  shader semantic tests now cover genuine Apple AIR. Native macOS translation
  and validation pass. Actual Mesa NIR/ACO SPIR-V-to-GFX11 compilation now passes
  on Windows and native macOS, with identical machine code and metadata. The newly
  emitted ACO binary has not been executed. Unsupported shaders return errors.
- Main agent: live target evidence, native integration and validation, immutable
  follow-up capture package, normal GPU/audio operation gate, and retirement
  coordination. Gate and callback retirement are implemented and native-linked;
  Device binding and downstream guard integration are native-linked; target
  activation and callback/recovery evidence remain.
- Display worker (`capture_display`): traced the actual DOMAIN0 capture failure,
  added the missing raw-read evidence without additional MMIO, and reviewed the
  normal-operation gate. FRL training and HPO link protocols are now in builds;
  PHY/DMCUB, FRL stream and DSC integration remain. Actual GCAP-based HDA stop,
  command-ring/position-DMA shutdown and held controller reset are implemented
  and tested. The reviewed native FunctionGroup binding checks per-I/O
  revocation and is part of the successful 78-source linked build.
- Auxiliary worker (`capture_aux`): traced VCN0's all-ones PGFSM to the actual
  discovery-derived address. The source-backed early SMU media power protocol
  is implemented, tested and included in builds, without native activation.
  Engine now separates firmware-first initialization from retirement with the
  real one-use token and distinct LSDMA observation. Untouched VCN/JPEG is
  deferred to post-PSP use; early wake remains disconnected. The full diagnostic
  checker is unchanged. No unavailable register value is a stop proof.

Keep worker file ownership separate. The main agent reviews changes, adds
completed modules to every build path, and verifies the combined tree. Recheck
live agent state after interruptions; historical assignments do not establish
that a worker or build is still running.

### 1. Establish a native baseline

Use Windows for the current checkout and the existing Xcode/Python installation
on `victor@mini` for native builds. Run the tests and remote build described in
`HANDOFF.md`; record compiler, SDK, OS, source commit/dirty state and outcomes.
Resolve actual compiler failures. Preserve firmware hashes, ROM, discovery
sample and source pins. No completed migration/import is assumed. Match each
build report to its recorded source snapshot before calling it current.

### 2. Complete PCI, power, reset, and DMA ownership

Review `HackyNaviDevice`, `HackyNaviEngineController`,
`HackyNaviResetController`, `HackyNaviAtomController`, and GART owners together.
Before calling engine startup from the PCI service, establish exclusion across
start/stop, reset, shutdown, removal, and IOKit power transitions. Mode-1 reset
also requires exclusion against the sibling HDMI audio function. The actual
paired owner, audio stop and retirement contract are now implemented and
reviewed; hardware validation remains outstanding.

Retaining a C++ object does not veto upstream power removal. Do not acknowledge
a power-down transition while pretending unretired DMA is safe. Design the
actual bounded stop/reset/PM handshake and its failure behavior against the
IOKit implementation and real hardware constraints. Keep MMIO inaccessible
outside the acquired power state. Never free memory still potentially referenced
by any engine or display block.

The current uncommitted diagnostic-service changes add explicit termination
callbacks and discard/remap BAR5 per powered generation. These do not provide
active-engine DMA retirement. XNU treats a `setPowerState` error as an ACK and
can time out a delayed ACK; neither vetoes parent power loss. Closing an
ordinary kext's PCI provider also does not prove bus mastering stopped.

The normal `FunctionGroup::Operation` now holds a real shared gate across a
CPU operation. Both function callbacks publish revocation before waiting on
it; shutdown stops new admission before waiting on any lock. Normal scope
validation is thread-bound and uses a bounded, explicit PM capability/D0
check. It must not enter inherited/configuration workloops synchronously.
Do not pass this scope as permission to run MODE1/restoreDeviceState. The
separate callback-scoped retirement coordinator is now implemented and linked.
Its actual Device handler binding and descendant guards are now connected.
The reviewed native build is `build/macos-device-engines-final`; first target
activation still requires resolving the incomplete boot-display observation.

Cold startup now explicitly clears and verifies BME before allocating/publishing
GART storage, installs MMHUB with BME disabled, then enables and verifies BME
before firmware traffic. Runtime GFX/flush checks still require BME enabled.
The EFI console's actual platform metadata now joins the VRAM reservation
union, including mapping padding and XNU's double page rounding. These changes do not replace the
remaining target cold-start and lifecycle validation. Device activation now
uses an additional explicit opt-in and is outside the delivered capture test.
The source-backed next sequence is in [cold-boot admission](docs/cold-boot-admission.md):
defer untouched VCN/JPEG on a genuine one-use firmware-first attach, keep LSDMA
and other boot-producer checks, and preserve full retirement on later failure.
Do not connect the experimental early-media wake merely to make PGFSM readable.

The live boot in `docs/validation-2026-10-11-live-capture.md` validates capture
on 15.8.1 with HDMI and no reported incident. It also exposes two real startup
obstacles: omitted DOMAIN0 status data and unreadable VCN0 PGFSM before media
domain power-up. Resolve these without bypassing the stopped-state predicate.

The existing reset ownership transfer sequence is:

```
reset.reset()
atom.initializeAfterMode1()
reset.verifyRetirement(atom)
proof = reset.retirementProof()
engine.releaseAfterVerifiedReset(*proof)
atom.releaseAfterVerifiedReset(*proof)
reset.finishRetirementTransfer(*proof)
```

Inspect real signatures and preconditions before using it. A failed proof pins
resources; “reset requested” is not “reset completed.” Avoid changing these
contracts just to make startup compile.

### 3. Complete scanout and modeset

Native DAL/DTO clock ownership and fresh all-pipe idle admission are now in the
working tree. Complete source-backed DCN3.2 global DISPCLK/DPPCLK management
with validated power-gating transitions and the modeset policy. The new
DPP DTO accepts an actual established reference frequency; EDID pixel clock
is not a substitute. Hold DCCG/PM exclusion and disconnect the selected DPP
from every MPC output before configuring it.

DCN3.2 HUBP0..3 power control uses DOMAIN0..3. The pinned DCN3.2 sequencer
has no separate DPP power-gate callback. `display_power` now implements real
IP_REQUEST_EN/PGFSM transitions; native display ownership retains these domains.
Do not carry forward older even/odd HUBP/DPP domain numbering.

Implement the real HUBP surface/address/pitch/format programming, DPP processing,
MPC routing, and OPP pipeline, with owned and pinned scanout memory. Calculate
bandwidth/watermarks/global-sync inputs from the correct DCN3.2 model; do not
invent conservative-looking constants or reuse unrelated ASIC tables.

Portable HUBP surface/flip/drain, DPP/MPC/OPP routing and HUBP-clock ownership
are now implemented and in the build lists. Their native allocation/modeset
integration and real HUBP initial translation/requestor setup remain. They do
not calculate DML bandwidth or establish visible output by themselves.

`userspace/display` now executes the unmodified pinned AMD DML32 formulas for
one progressive linear RGB32 surface and RGB24 HDMI/legacy DP. Sanitized tests
cover 1080p/2160p, real latency response and exact requested link capacity.
This is an internal trusted-profile calculation API. Integer encoding now
preserves generation/geometry/reference-clock provenance and four explicit
watermark calculation records; the kernel validates exact register masks.
Next bind actual ROM/SMU clock and latency inputs, preserve explicit units and
derive the real firmware watermark policies. Connect the implemented DET,
watermark and RQ/DLG/TTU controller through the native modeset owner's gate.
Do not run floating-point DML in the kernel or silently use fixture clocks.

Integrate clocks, pixel PLL, OTG, memory pipeline and physical link into a
transactional modeset owner. Shared PLL/pipe resources need explicit leases.
Currently shared PLL changes are rejected while HDMI streams retain hardware.
Complete native OTG and display-clock ownership with equivalent fault retention.

For HDMI, connect the existing AVI/VPG/HDMI stream controller only after the
real scanout path is established. For DisplayPort, add pixel stream/MSA,
transport timing and required packets: link training alone is not video.
Start with one supported progressive RGB mode and one connector, then expand
to all physical HDMI/DP connectors, hotplug, mode changes, multiple outputs,
and power transitions. Validate on the target board.

### 4. Expose native framebuffer and surfaces

Implement the macOS framebuffer/display interface, mode enumeration, hotplug,
scanout surfaces and presentation synchronization. Integrate IOSurface and
memory ownership through actual supported kernel/user boundaries. Identify
the applicable macOS 15 interface from SDK/XNU and actual system binaries;
do not claim compatibility based on method names copied from another release.

### 5. Expose rendering and Metal

Build a validated user/kernel command submission interface with per-client
memory ownership, address isolation, fences, cache synchronization, scheduling,
and reset recovery. The current trusted kernel IB path must not become an
unchecked user command transport.

Use the Mesa/RADV/ACO audit and Cobalt64 audit to select reusable components.
Implement the Darwin winsys and actual memory/synchronization/submit bridges;
Linux DRM ioctls are not available on macOS. Review licenses component by
component. Preserve provenance and notices for imports.

Implement the macOS accelerator/Metal frontend and compiler/resource/pipeline
interfaces, backed by real GPU commands. The NVIDIA project is an architectural
reference, not proof its frontend supports this backend. Deliver an actual
Metal device, buffer/texture operations, compute, graphics pipelines and
presentation. Unsupported operations must fail explicitly, not return fabricated
success. A bounded Metal frontend and real shader/submission paths now exist in
the tree and compile natively; accelerated rendering remains unverified on the
target and general application/presentation support is incomplete.

### 6. Hardware validation and completion criteria

HDR is unimplemented and explicitly deferred by the user. Prioritize boot,
real GPU execution, SDR display and Metal. Packed 10-bit formats do not imply
HDR support; do not spend the current implementation workstream on HDR.

With the target installation now reported available, prepare the driver and
its staged test procedure. Once the driver is ready for the proposed test,
collect PCI identity, build identity and logs before activation. Use staged
bring-up with bounded diagnostics and exact failure stages. Verify real fence
writes and shader output, scanout address ownership, physical HDMI/DP images,
hotplug, mode switches, and stop/reset/power behavior. Preserve enough evidence
to reproduce failures without assuming an SSH connection survives a panic.

Run real Metal rendering/compute and presentation tests, stress allocation and
cleanup, and test recovery without use-after-free or residual DMA. Completion
requires target-hardware evidence of acceleration and working HDMI/DP, not
passing host tests or a linked bundle. Track any still unsupported features
explicitly. Do not mark the overall driver goal complete prematurely.

## Working discipline

- Read existing code before adding duplicate subsystems. Keep platform-neutral
  protocols testable and native ownership explicit.
- Use pinned AMD primary sources and generated register masks. Distinguish
  source-derived constants from runtime board/sink data and measured values.
- Keep bounded waits and useful stage/status diagnostics. Test ambiguous I/O
  completion and retained ownership, not only successful register sequences.
- Preserve existing work; no force resets or broad cleanup of shared storage.
- Keep host tests, kernel compilation/linking, load validation, and hardware
  execution as separate evidence levels in reports.
- If migration is resumed, work locally on the Mac. Avoid accumulating remote source
  copies, dependency clones, SDKs, build directories, or unnecessary packages.
