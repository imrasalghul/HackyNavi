# Next combined graphics test: dependency and evidence audit

Audited 2026-10-11 against the current native startup, public Metal tools and
`live-3fa3f6d4723b-20261011-163144-730`. This is a source/evidence audit, not a
ready-to-boot claim. Hashes, the complete ordered `Engine::start` stage list,
source-derived cold-admission register tables and machine-readable gaps are in
`build/next-graphics-test-audit-1/report.json`.

Implementation follow-up: that JSON is the frozen initial audit, not a claim
that all gaps remain unchanged. The combined Device/Engine preflight, bootstrap
inventory and stage journal are now implemented and independently reviewed;
`build/macos-combined-graphics-preflight-1` passes full native compilation/link.
Metal loader/ABI reporting passes `build/graphics-admission-frontend-native-1`.
The runner verifies all eight test inputs, retains each command's arguments,
streams/exit and owner generation, and collects before/after evidence. Collection
command completeness is distinct from graphics and collector process status.
Native shell syntax passed `build/graphics-runner-native-syntax-2`; execution on
the target remains untested. Recovery preparation, partial reads and actual
firmware framebuffer ownership remain active work, as detailed in
[combined evidence](engine-combined-preflight-evidence.md).

**The next issued test must be a real engine/graphics attempt with diagnostics
batched into that same boot. No additional probe-only reboot is proposed.**
Current source is not ready to satisfy that requirement: the already measured
pipe0 TLB0 rejects the display preflight before any queue is started. Merely
adding `-hackynavi-engines` would reproduce a known early rejection. The
address/ownership implementation must be resolved before calling a package a
graphics test. Guards stay enabled; a combined run may stop at a genuine new
failure and retain its observations, without misreporting GPU execution.

## What 3fa has established

The target loaded core UUID `F2DDA1C3-D4B2-3C4E-AF63-E70C25298DAC` on
macOS15.8.1/24H32. It ran the device-capture mode, not engine mode. Discovery,
bound legacy ROM, memory layout, posted/SOS checks, original display registers
and all32 context registers were captured. VRAM is25,753,026,560 bytes; BAR0 is
256MiB at CPU physical `0x880000000`; framebuffer MC base is `0x8000000000`.

The additional208 reads completed with both valid masks `0xffffffff` and no
changes. Pipe0's VTG/OTG/current-master/clock/busy fields are enabled; pipes1–3
have disabled VTGs, disabled requested/current OTGs and idle/disabled OTG
clocks. Their HUBP functional clock enable is clear, although DISPCLK_R and
DCFCLK_R report on. Every HUBP is powered and has BLANK_EN=TTU_DISABLE=0.
MALL selections and SubVP start-line configuration are zero. VM context0 and
AGP_BASE are zero; that is not a TLB0 address-domain specification.

Pipe0 still reports linear RGB32 1920x1080, pitch1920, primary/EARLIEST address
`0x8000000000`, VMID0 and TLB0. Its ordinary INUSE latch is zero with NO_LATCH
clear. The only original raw difference from9eda is IN_BLANK status. These are
the firmware buffer geometry, not a measurement of a 4K165 desktop mode.
VCN0 PGFSM is all ones and remains unread/unclassified. No actual engine,
allocator, DMA mapper, firmware load, native user-client, Metal device, shader
dispatch or rendering result is present.

## Required execution chain and measurements

The early core is loaded through OpenCore/BootKC. The separate actual
IOAccelerator subclass requires late IOGraphics availability and must not be
put in OpenCore Kernel/Add. Its successful load plus the matching Metal bundle
is necessary for public Metal discovery. A core kext alone cannot run the
public Metal test. Every row below is required by the current implementation,
unless explicitly marked informational or deferred.

| Dependency, in execution order | Existing target evidence | What must execute or be measured in the combined run |
| --- | --- | --- |
| Exact package and target identity | 3fa core loaded; target15.8.1/24H32; real1002:744c/revC8 | Hash/UUID receipts for the newly built core, late accelerator, bundle, helpers and fixtures; actual loaded core/aux UUIDs and exact boot arguments. The existing3fa ZIP excludes aux and Metal. |
| Device worker and paired FunctionGroup | Graphics GFX0 and HDAU sibling in service tree | Exact same-slot functions0/1, expected identities, two actual exclusive leases, both powered, assigned BAR5/map equality, unrevo­ked normal Operation and nonzero generation. Record acquisition/PM failures without inventing an epoch. |
| Recovery preparation before Engine start | Bound ROM/discovery and normal PCI identity from diagnostics | Actual reset-controller discovery/ROM/profile construction, PCI/config/BAR/PM/PCIe snapshot comparison, SOS/Scratch3/Scratch7 observations, ATOM ASIC_Init profile acceptance and registered retirement handler. Profile availability is not a reset/recovery success. |
| Fresh discovery/ROM/memory/POST | Prior complete3fa capture | Repeat under this engine Operation. Bind binary bytes/hash, discovery versions/bases, live ROM image/subsystem, firmware/VRAM-usage fields, BAR0 resource/config and FB base/top/offset. Retain partial validity. Existing selector writes are distinct from engine-control/DMA writes. |
| Training and first-attach admission | SOS0 measured; C2PMSG35 unmeasured | Two C2PMSG35=`0x80000000` observations bracketed by SOS0, and the real one-use FunctionGroup FirstAttach. Record all words/read status, not only a generic training error. |
| LSDMA cold admission | Not captured | Two guarded observations of all12 LSDMA predicates and SOS. Record each actual word, offset, mask/expected and pass. A failed predicate must not conceal independent safe observations. |
| Display preservation and console preflight | Raw display/context known; classification rejects pipe0 | Implement the actual TLB0/firmware-preservation ownership path first. Then capture full raw display/context and both console tuples under current access even when another semantic predicate fails. Keep negative preflight distinct from owned recapture. |
| Sibling audio retirement | Topology only | Stop interrupts/streams/CORB/RIRB/position DMA/controller, withdraw sibling BME and observe nonposted Transactions Pending clear; require no other audio clients. Preserve AudioStopReport including underlying controller stage/last register. This is the first possible engine-control write in current Engine startup. |
| Inherited DMCUB stop | Not captured | Read current controls/GPINT state, actual firmware-stop handshake where active, reset/disable plus window/ring-pointer readbacks. Preserve response/timeout/partial-write evidence. This can affect visible scanout even though no mode set is requested. |
| Backend observation | Surface/timing only; no complete backend route | Existing bounded BootBackendCapture after actual DMCUB stop: ATOM path, PHY/backend/frontend/timing/OPP/MPCC/DPP, format and link state. Unsupported DP/FRL/TMDS profile is informational for offscreen work; access loss/read error/instability/deadline remains fatal. |
| Owned boot display capture | Absent | Recapture/revalidate after ownership transition, retain all active scanout ranges, and establish dormant-HUBP treatment. No diagnostic success substitutes for the actual bootdisplaystop::State/phase. |
| CP/SDMA cold stop | Not captured | Check MES/VM/context/SDMA queue guards; halt actual CP/MEC architecture and both SDMA engines; verify control readbacks plus CP/GRBM idle. Capture pre-state and post-state separately. |
| LSDMA recheck and remaining DMA | Only MALL_CONFIG/SUBVP subset known | Fresh LSDMA recheck; after DMCUB stop, both passes of11 IH,18 DWB/MCIF fields, global MALL and per-HUBP MALL status/VMPG/start-line/selection plus all power guards. Equal IH ring pointers, clean counters and unarmed triggers are separate predicates. |
| Console recapture and reservations | No physical console tuple/ranges | Stable classified physical console range and full framebuffer inventory; ROM firmware/driver usage, discovery tail and training exclusions; actual reservation union and completeness/missing-evidence bits before allocation. Reserve full console mapping, not just visible pixels. |
| BME off, NBIO, GART storage | BAR layout only | Actual graphics BME withdrawal/readback; NBIO setup; disjoint GPU aperture; real DMA mapper/segments/prepared pages and owned VRAM allocation addresses; upload/HDP visibility. Default GART aperture512MiB; data64KiB, writeback4KiB, display512KiB plus real default/fault page and page table. No address synthesized from CPU virtual pointers. |
| MMHUB, BME on, doorbell | Not captured | Program/invalidate/posting completion under actual GART Access, then real BME enable/readback and NBIO doorbell aperture. Record all result/stage/register values and retention on ambiguous failures. |
| SOS, PSP ring, boot config, TMR, graphics firmware | Prior cold SOS0 only | Actual signed firmware parsing/upload, bootloader acknowledgments/SOS progress; PSP ring fence/response; boot-configuration result; requested/allocated TMR alignment/size; every graphics firmware load/autoload result and SMU state. Static resource hashes are offline preparation, acknowledgments are runtime evidence. |
| GFXHUB, graphics prepare/start | Not captured | GFXHUB invalidation; graphics allocation/clear state/topology/RLC setup; queue buffers/doorbell/ring configuration and startup fence/read pointer. Capture real topology and firmware/init/queue result structures. |
| Kernel GPU verification | Not attempted | CP write/data verification; built-in GFX11 clear/copy shader verification; two-job reusable Apple-add-one fixture path with actual fence/read pointers, data/guard words, stale-result rejections and preserved VM/data owners. CP fence alone is not shader output. |
| DMCUB owned boot and Engine Complete | Not attempted | Actual loaded DAL DMCUB version, valid runtime windows, mailbox open and successful DisplayBoot. Current Engine marks Ready only after this stage, even if compute already passed. Clock policy journals are required only if actually requested. |
| Late accelerator and public loader | Aux/bundle not installed by3fa package | Late kext loads actual IOAccelerator; exact child/provider/PCI relation and generation; MetalPluginName/ClassName; accepted bundle path and linked libraries/helpers. Capture loader rejection reasons, not only empty MTLCopyAllDevices. |
| Metal ABI and native v1.7 admission | Measured/build-tested on mini24H22, not target24H32 | Actual native-macOS dyld platform1; _MTLDevice superclass/limits type encoding; exact required MTLDeviceSPI method set/encodings; real accelerator user-client open; Hello1.7 identity/epoch/capabilities/page/VA/budget limits; GraphicsInformation topology and color-target admission. |
| Public compute test | Tool compiled only | Public MTLCopyAllDevices selects exact HNMetalDevice registryID; genuine add_one metallib/AIR compilation; three Managed BOs; A→B→C GPU submissions,4099 results and64 guards, ordered synchronize/readback and exactly-once callbacks. Save exit/stdout/stderr and phase. |
| Public pixel tests | Four native tools compiled only | Constant render64x64 RGBA8 then fragment-buffer and vertex/combined-buffer cases; real public Metal pipeline/texture/queue, GPU draw, ordered synchronization, actual readback pixels/orientation/padding guards/hash. Two different source uploads must produce different verified geometry/results. |
| Retirement on any failure | Source/native build evidence only | Current stage and all previously completed stage evidence must survive. Real failure recovery may need MODE1+ATOM ASIC_Init+physical retirement; retain storage on uncertain result. One cold attempt, no automatic rearm;5-second outer retirement budget and terminal-callback restrictions remain. |

`Stage::Auxiliary` occurs twice and is not the same observation. Preserve both.
The complete ordered stage list is mechanically extracted into the JSON; a
single final `EngineStage` is insufficient to distinguish what actually ran.

## Exact missing preflight observations

These are observations to batch into the graphics run, not another test request.
They must be independently attempted only while the real Operation, D0,
mapping and each domain's access prerequisites remain valid. Continue after a
semantic mismatch when the next independent read remains justified; stop on
revocation, transport failure or unknown power. Never convert unread zeros or
all ones into a valid observation. Do not wake or probe gated media windows.

1. **Training:** MP0 BASE_IDX0+`0x63` C2PMSG35, plus repeated BASE_IDX0+`0x91`
   SOS reads. Current failure logging omits the actual command history.
2. **LSDMA:** the twelve `lsdmaFields` in `src/cold_aux.cpp`, with the original
   offsets/masks/expected values copied into the JSON. Includes both queue
   controls, automatic context/world switching, PIO status, engine/MC/copy idle,
   and translation/write-data queue conditions. Bracket with SOS0.
3. **CP/SDMA negative admission:** MES control,16 GFX context controls,16 MM
   context controls, both SDMA instances' queue1–7 RB/IB enables, CP/MEC
   architecture bits and actual ME/MEC/queue0/F32 controls. The concrete77
   address formulas from `bootstrap_quiesce.cpp` are in the JSON. Pre-read
   values are not proof that halt/drain has completed.
4. **Display:** all original global/VGA/HUBP words and power guards, plus the32
   context words. Capture the raw state separately from classification. Current
   `captureDisplayPreflight` discards the raw snapshot and context is not
   connected to Engine startup. Thus simply enabling engines loses useful
   diagnostics currently present in capture mode.
5. **CPU console provenance:** raw base, address-kind/provenance, display value,
   width/height/depth/rowBytes/offset/length/rotation, both read results and
   stability, actual GPU PCI registry identity and assigned BAR0 range, and
   derived complete mapping/rounded exclusion interval if admitted. Preserve
   why a tuple cannot be classified. The3fa tree's `.Display_boot` association
   is useful but is not the buffer's physical address.
6. **DMCUB:** initial control/reset/GPINT/pwait/scratch state and the actual
   windows/pointers touched by the inherited stop protocol. The JSON maps its
  45 declared register words. This is a bounded source catalogue, not an
   instruction to read every word indiscriminately before its documented
   access prerequisites. Stop-handshake response and reset postconditions can
   only be obtained during the same run's actual transition.
7. **Other DMA:**11 IH and18 DWB/MCIF words plus global MALL, each HUBP's
   config/status/subviewport/VMPG and power triplets. Only MALL config/subviewport
   and power were measured in3fa. The real `colddma::checkStopped` contract
   requires DMCUB stopped/reset and cold SOS; do not turn an early raw sample
   into this proof. Gated IH/DWB SRAM or held reset are rejected, not idle.
8. **Native lifetime and recovery:** paired function identity/BAR/PM results,
   generation, exact failed factory, reset preparation and ATOM profile result.
   Failed early FunctionGroup admission is otherwise just a waiting state, and
   an early recovery-preparation failure currently precedes Engine observations.

VCN/JPEG cold-stop data is **not** a prerequisite of this one-use first-attach
path. The existing FirstAttach contract defers untouched media while retaining
all LSDMA/CP/SDMA/IH/writeback/DMCUB/reservation checks. Do not add a gated VCN
read requirement simply because the passive inventory returns all ones. Real
reset/recovery after publication has a different, stronger retirement contract.

## Address provenance and dormant HUBPs

The new target evidence resolves which timing generators are stopped. It does
not allow deleting HUBPs1–3 from the memory-owner inventory: Linux
`dcn10_init_pipes` still disconnects/disables their planes even when the TG was
already disabled. Current bootdisplaystop capture requires a fully classified
snapshot before arming; current startup never calls its stop operation on the
successful inherited-scanout path. A source-backed native handoff/normalization
for dormant requesters is therefore implementation work, not a collector fix.

For active pipe0, the normal MC→VRAM offset-zero arithmetic is plausible and
corroborated by Linux's firmware reservation convention, but the specific
DCN3.2 TLB0 transformation remains unproven. Zero VM context words do not
resolve it. A same-boot physical firmware framebuffer tuple is independent
corroboration, not a general rule accepting all TLB0 surfaces.

`HackyNaviBootConsole::capture` already reads IOPlatformExpert console info
twice and rejects canonical kernel virtual pointers, classifying an untagged
base only when inside real BAR0. Late IOFramebuffer console updates can replace
the original physical base. Thus the next implementation must preserve
immutable original boot video/GOP provenance or use an actually observed,
unambiguous on-BAR physical tuple; do not dereference/guess a virtual address.
This source path and its limitations are recorded in
`build/boot-display-pa-research-1/report.md`. Current Engine display rejection
prevents even this existing console getter from running; batching fixes missing
evidence, not missing ownership. The full console mapping remains reserved
until a real console handoff because CPU console writes may continue.

## Collector and publication gaps to close before release

- **Engine mode bypasses capture mode.** `HackyNaviDevice::captureWorker` takes
  the engine branch immediately. Add the bounded preflight batch to the actual
  engine route; do not start a concurrent second MMIO owner. Use persistent
  heap-owned bounded capture storage and the same actual Operation. Large POD
  evidence must not be added to kernel stack frames.
- **Early exit hides independent causes.** Display preflight currently returns
  on TLB0 before console; Training/LSDMA can exit even earlier. Record independent
  safely readable negative preflights before the control-write commit point,
  then evaluate preserved guards. Access failures remain terminal.
- **Success/failure results are incomplete.** Current Observation publishes
  preflight-display summaries, console tuples, backend classification, scanout
  summary, CP/shader/reuse checks and retirement summaries. Training, full
  LSDMA, audio subprotocol, cold-DMA arrays, actual reservation ranges, GART,
  PSP/SMU/firmware/queue and recovery preparation details mostly remain internal
  or failure-only IOLog. Add bounded stage journals with attempted/succeeded/
  valid masks, raw last values, protocol status, time/generation and mutation/
  retention flags. Retain startup journal separately from retirement.
- **Publication is currently after synchronous attempt return.** An early panic
  may prevent LastEngineObservation publication. Emit bounded stage progress
  and preserve a safe pre-control snapshot before mutation; final collector alone
  cannot recover vanished memory. Do not promise persistence from ordinary
  IORegistry properties; collect available panic/serial/OpenCore evidence.
- **Generic collector at audit start described passive/live diagnostic packages.** It
  recorded12 capture commands but neither accelerator properties nor user-space
  tool output. Extend the graphics-specific collector/runner to include actual
  core and aux loaded identities, accelerator and parent properties, engine and
  retirement observations, package-file hash receipts, tool arguments/times/
  exit codes/stdout/stderr and before/after engine state. Distinguish test not
  attempted, skipped by a failed prerequisite, timeout, and actual test failure.
- **Loader failures are not kernel-only.** Current unified-log predicate is
  `process == kernel AND message contains HackyNavi`; it misses Metal/dyld/
  AMFI/helper-process errors. Add bounded relevant user-space loader logs and
  actual test errors. The principal init entry currently discards its NSError
  when returning nil, so a targeted bounded failure reason is needed to avoid
  treating empty Metal enumeration as a GPU failure. No system policy change
  is implied by gathering those reasons.
- **Runtime ABI acceptance is unmeasured.** Capture actual target SPI/limits
  encoding mismatch details in the same run if admission fails. Mini24H22's
  measurements cannot certify target24H32. Preserve exact v1.7 Hello/graphics
  topology and registry/provider relationship, not a forged compatibility
  profile or fallback device.
- **Test coverage must include output.** Compute currently emits PASS/FAIL text,
  while render tools emit JSON phases and real pixel hashes. The runner must
  keep both streams and command exits; ideally compute gains comparable phase
  reporting. Snapshot exact tool/metallib hashes before execution. CPU initial
  data and comparison oracles are permitted; CPU-generated expected pixels must
  never be substituted for texture readback.
- **Single-run ordering:** after successful Engine Complete and exact public
  device discovery, run compute, constant render, fragment-buffer render,
  vertex/combined-buffer render serially, collecting state after each. A lost
  epoch/device or recovery failure prevents further submissions; preserve all
  earlier results and explicit skip reasons. A shader/pixel mismatch should
  stop escalation, not trigger reset/retry loops that erase the first failure.
- **Actual shutdown/recovery evidence:** preserve journal and crash evidence
  when available. Existing reset permission/recovery deadline/fail-stop behavior
  is not a successful target test. Do not add a sleep/wake requirement to the
  first offscreen proof, and do not infer either from a normal desktop boot.

## What can be finished without another target reboot

Resolve the exact firmware-display preservation/dormant-requester path; integrate
all bounded evidence above; make the late accelerator and bundled public tests
one coherent versioned package; validate native kernel imports/stack/build and
userspace dependencies; check the collector against saved complete/partial/error
evidence. The real graphics attempt then supplies runtime firmware/DMA/queue/
shader/pixel results in one boot. No preflight register dump can prove those
post-write behaviors in advance.

This first offscreen result would establish actual Metal compute/rendering on
the target if it passes. WindowServer acceleration, scanout handoff/modes,
working HDMI/DP links/audio, FRL/DSC and broad Metal application support remain
parts of the full driver objective. Neither a kernel CP test nor an offscreen
triangle alone completes that objective. HDR remains deferred.
