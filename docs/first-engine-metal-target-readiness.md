# First engine boot and offscreen Metal test

**Latest result and test constraint:** `3fa3f6d4723b` has now loaded on the
actual 24H32 target with the expected UUID. Its 32-word context is complete and
stable: only timing0 is running; timing1–3 and their functional HUBP clocks are
disabled. Active pipe0 TLB0 mapping and retirement of the dormant requesters
still need an implementation backed by the actual hardware contract. Repeating
the context capture will not answer that mapping question. The user requires
the next requested boot to attempt actual graphics and to collect all necessary
diagnostics in the same run. No further probe-only package is to be issued.

**2026-10-11 follow-up:** the replacement `9eda6a40f88c` core now loads on the
actual 24H32 target, but its newly complete display register capture is rejected
by the inherited-address decoder. The earlier conclusion below about no
observed deterministic startup rejection is superseded. Resolve the
source-backed AGP/TLB/flip/in-use interpretation and scanout reservation before
an engine test. See [new target evidence](validation-2026-10-11-9eda-loader-success.md).

The completed capture package is
`build/releases/display-context-20261011/HackyNavi-live-capture-3fa3f6d4723b.zip`,
built at 16:05 America/New_York. It adds a guarded two-pass inventory of 32
timing, HUBP and VM-context registers, with engines disabled. The four
source-backed decoder corrections do not resolve the observed TLB0 semantics
or powered/unblanked zero-viewport pipes. See
[address audit](boot-display-address-audit.md) and
[new capture scope](boot-display-context-capture.md).
Native v1.7 transport, vertex-buffer compiler/runtime, Metal frontend and its
geometry-readback tool have now compiled and passed their bounded offline
checks. They remain untested on the target. The following table is the older
capture's audit; the current boot succeeds but display reservation remains open.

Read-only audit of the current development source and the supplied
`E:/HackyNavi-diagnostics-20261010-232322` capture. No target configuration,
installation, register, firmware or GPU operation was performed by this audit.
Source/evidence hashes are bound by `build/engine-first-target-readiness/review.json`.

**Full HDMI takeover is not a prerequisite for this offscreen path.** There is
no demonstrated deterministic startup failure in the supplied observations
after the current power-state correction. There is also no target evidence
that the current engine path, public Metal discovery or GPU rendering succeeds.
The first engine boot is experimental bring-up with hardware writes and a
real failure-recovery path; successful compilation does not make it a
non-disruptive diagnostic boot.

## Existing evidence and the next gates

The supplied boot is macOS 15.8.1/24H32, with `-hackynavi-device` and
`-radvesa`, without `-hackynavi-engines`. Its loaded binary is the older
`dbbcb5ec0ba9` capture build. It reports valid discovery, a bound 57,856-byte
legacy ROM and 25,753,026,560 bytes of VRAM/256 MiB BAR0, but explicitly says reservations are
not established. All four power probes report stable STATUS `0x10000000`,
CONFIG0; the old classifier stops there, before any complete surface inventory.
VCN0 PGFSM is all ones. No queue, shader, reusable-job or engine test ran.

The separate issued `45eab551a719` diagnostic corrects that power predicate and
adds bounded VCN POWER_STATUS observations. This audit found no supplied result
from that exact diagnostic. Its existing handoff keeps engines disabled. Its
purpose is to reveal the previously unread display registers, not to certify
DMA or become the new engine-test binary.

| Current prerequisite, in order | Evidence still needed on the target |
| --- | --- |
| Paired graphics/audio exclusive opens, real powered FunctionGroup, BAR5 mapping | The supplied service tree shows graphics and sibling HDAU, but not the engine-only leases or their PM admission. |
| Recovery owner preparation, before `Engine::start` | Real fresh PCI/config snapshot and prepared ATOM profile. The supplied ROM's selected legacy image independently matches the exact 132-byte ASIC_Init CRC `0xc5fb8999`; the log does not retain its live bytes or prove execution. |
| Discovery/ROM/memory/posted admission, then `Training` and LSDMA | Fresh exact PSP C2PMSG35 `0x80000000` bracketed by SOS0 and a stopped LSDMA observation. The captured SOS0 alone is insufficient. |
| Display/console negative preflight | A full stable supported linear packed-RGB, VMID0, non-DCC surface footprint and console reservation. Current source accepts the observed power pair, but none of those later surface fields were read in the supplied log. |
| Audio stop, inherited DMCUB stop, owned display capture, CP/SDMA and remaining DMA checks, reservations | Actual completion/readbacks. New VCN inventory success is not required as a substitute: first-attach startup separately defers untouched media and still requires LSDMA and the other actual cold checks. |
| NBIO/GART/MMHUB/BME/PSP/TMR/graphics firmware/GFXHUB/queue startup | Actual stage results and physical observations; none are present in the diagnostic capture. |
| CP write, Apple compute shader and two-job reuse verification, then DMCUB boot/mailbox | All three real GPU verifications and DisplayBoot must pass before Engine Ready and accelerator publication. |
| Public Metal bundle discovery, exact native v1.6 negotiation and offscreen tests | Actual loader/runtime acceptance and pixel readback on 24H32; native build-host objects and CPU compiler tests prove neither. |

The first unmeasured *engine-specific register predicate* after the already
observed basic inventory is `Training`; real FunctionGroup and recovery
preparation execute before it and are independently unverified. A later stage
cannot be predicted from the old classifier's early rejection.

## Why downstream display work does not block this test

`Engine::start` captures and reserves inherited scanout/console memory without
calling `bootdisplaystop::stop` during successful startup. The new backend
observer adds no writes or ownership: a stable unsupported DP/FRL/HDMI format
returns Unsupported and startup continues. Transport errors, changed samples,
lost access and deadlines stop startup. No HDMI-TMDS result grants authority
over a 4K165 FRL or DP-DSC link.

Startup does not call the cold global DISP/DPP, UCLK, SOC/FCLK, DS/DF, FAMS,
OPP-blank, MPC/OTG-stop or inherited-stream-mute transactions. `Display::boot`
requires clock journals only if a real earlier request exists. Reordering
startup merely to satisfy those unused cold-clock APIs is unnecessary here.
It still stops inherited DMCUB and later boots its own DMCUB: preserving the
scanout allocation is not proof that those firmware operations leave the
visible desktop undisturbed.

The supplied tree contains `.Display_boot`/IONDRVFramebuffer, AMDSupport,
AppleMCCS and AGPM clients. The exclusive PCI open and FunctionGroup serialize
the driver's actual PCI/power lifecycle; they do not prove that previously
established display mappings or external firmware never act. Reserving the
complete console allocation addresses continuing CPU console writes. No
additional writer was observed by this audit, and no concurrent-writer defect
is inferred solely from a service name. Continued display stability remains
a target observation.

## Concrete failure and retirement boundary

`StopSiblingAudio` marks the first possible control write. Any later ambiguous
failure retains actual hardware owners. Before healthy `FirmwareLoaded` plus
GART `BothEnabled`, Engine cannot use orderly retirement and the lifecycle
requires genuine MODE1, ASIC_Init and physical retirement verification. The
later no-queue branch additionally requires actual zero CSIB/disabled SRM;
Prepared/Running queue branches keep their own stronger ownership checks.
None is an unconditional recovery guarantee.

Failed activation automatically requests OwnerFailure retirement. Reset is
permitted for that reason or PowerDown, but not close, termination or shutdown;
intersecting terminal callbacks, a failed proof or the cumulative five-second
retirement deadline invoke the existing fail-stop/panic. Inner poll limits and
the longer ATOM interpreter limit do not extend that outer deadline. This is a
source-visible test risk, not a reported panic from the submitted diagnostic.
Successful boot also does not establish sleep/wake: this is a one-shot cold
attempt with no automatic restart after retirement.

Ordinary stop still orders inherited HUBP drain, Display consumers, queue/VM,
RLC, coordinated clock release while DMCUB is live, DMCUB stop, remaining
firmware clock/PSP retirement, GART, NBIO and BME withdrawal. It does not require
full OTG/MPC/DIO idle when no such clock/domain policy is owned. This preserves
the inherited-display offscreen route without weakening the coordinated-clock
guards. Outstanding or ambiguous state must remain retained.

## Evidence that separates each result

The engine test needs the exact loaded binary UUID/hash association, complete
`LastEngineObservation`, stage/register diagnostics and any
`LastRetirementObservation` or panic evidence. `EngineStartupVerified=true`
and Engine `Complete` mean the actual startup checks finished; they do not mean
public Metal discovery succeeded. The genuine `HackyNaviAccelerator` child is
published only after that success.

Metal then needs the matching packaged bundle/libraries/helpers, accepted
loader path/security policy and exact runtime SPI/limits encodings on the
target. Current native admission rejects incompatible encodings. The measured
15.8/24H22 loader contract is not a measurement of the target's 15.8.1/24H32
runtime. Source compilation is not needed by the supplied tests: they use
the retained genuine Apple metallibs. The explicit public compute, constant
render and fragment-buffer render smoke results distinguish discovery,
submission, synchronization and actual output verification. The latter two
check actual pixels, orientation and guards.

HDR, desktop acceleration, display-mode changes, HDMI audio restoration,
FRL/DSC takeover, general vertex/texture resources and broad Metal feature sets
remain separate features. They are neither evidence supplied by these tests
nor prerequisites for the bounded offscreen triangle. No production gate or
test setting was changed in this audit.
