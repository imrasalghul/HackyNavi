# Native client and display-input integration

This records source implementation and build evidence. No target GPU job,
controlled scanout or Metal device has been demonstrated by this integration.
The target's earlier HDMI capture boot had no reported incident. Its evidence
belongs to that exact capture executable, not these newer sources.

## Client path

The Device creates a real ClientSession through its retained Lifecycle/Engine
under a fresh FunctionGroup Operation. The factory requires the actual running
queue, memory layout and GART access guard. Each session owns a VMID, isolated
VA mappings and generation-checked native buffer handles. Every hardware-facing
session call reacquires the real group/GART scope.

The task-bound IOUserClient negotiates an explicit ABI v1.1 built-in compute
profile. It supports real allocation, isolated mapping, copied upload/readback,
one clear or copy dispatch, historical completion, and explicit close. Messages
contain at most 2048 copied payload bytes. No raw command buffers, physical
addresses, arbitrary shaders or CPU mappings are exposed by this profile.
The older generic transport continues to require its full capability set and
returns Unsupported rather than treating this smaller interface as equivalent.

Compute reserves its immutable CPU result before any GPU publication. The
same group Operation spans real submission, GPU-written fence observation,
GPU result readback and normal VM retirement. Only success through all those
steps publishes the sequence/result. This is a synchronous, sealed one-job
connection, not a reusable asynchronous Metal queue.

Normal retirement now checks the captured queue/producer/sequence, CP idle,
read pointer and fence before detaching both VM hubs and releasing storage.
Engine teardown also reclaims unpublished or proven-complete canonical jobs
after stopping the queue. A VM-disable acknowledgement alone is insufficient;
ambiguous exposed resources remain retained for the existing recovery path.

Close/death does not invent GPU completion or discard exposed ownership.
The explicit Close RPC reports native cleanup errors because ordinary
IOServiceClose discards the clientClose return value in the pinned XNU source.
Native owner references are dropped outside the client state lock.

Review identified and fixed unbounded accumulation of copied results after
VMID reuse. The production session now reserves from one atomic global budget
of 64 MiB and 64 retained-result holders before allocation. A noncopyable token
binds the exact byte count and issuing budget. Actual memory is freed before
returning its reservation on allocation failure, compute failure, close and
destruction. Denial returns NoResources before sealing or publishing the VM.

## Display inputs

The native Display observation now joins actual retained ROM board metadata,
seven twice-read DAL clock tables and DC ceilings, and twice-read PLL/timer
programming under one scoped deadline. The CPU adapter derives the DCN3.2
model using pinned AMD policy and those inputs. It does not substitute model
defaults for missing topology/VCO data, correlate independent DPM indices,
or treat programmed clock derivations as independent frequency measurements.

The supplied ROM parses as 24 channels of 2 bytes, a 100 MHz crystal and
zero raw memory/self-refresh latency overrides. Zero selects the explicit
ASIC policy; nonzero overrides currently fail because the pinned table-unit
comment and driver conversion disagree. The actual DML calculator and integer
register encoder remain userspace code. Their CPU tests are not modesets.

Clock-minimum changes invalidate native input observations. A future modeset
transaction must acquire all required clock holds with pipes off, recapture
and recalculate, retain authorized results, and then program bandwidth,
timing, surface and link state. This bridge, complete watermark/transition
policy, framebuffer/IOSurface, FRL/DSC and Metal remain unfinished. See
[the exact profile contract](../userspace/display/PROFILE.md).

## Focused evidence

- `build/native-client-result-budget/report.json`: real Kernel SDK Session
  object; 2,195 ASan/UBSan assertions, including 64,000 accounting attempts
  across 16 concurrent threads. No failures; remote stage removed.

- `build/native-vm-completed-retirement-final/report.json`: real Kernel SDK
  objects and 5,156 ASan/UBSan queue checks; remote stage removed.
- `build/native-userclient-v11-final/report.json`: real Kernel SDK UserClient
  and protocol objects; remote stage removed.
- `build/native-transport-builtin-v11-final/report.json`: native userspace
  library, probe and real compute smoke executable linked; 135,785 ASan/UBSan
  protocol checks passed. The GPU smoke executable was not run.
- `build/native-display-profile/report.json`: actual macOS CPU DML calculations,
  5,526 encoding checks and 195 profile checks; remote stage removed.
- `build/display-model-profile-report.json`: 725,284 DPM checks, 5,995 SMU
  checks and 8,634 model-input checks including the supplied ROM. Real Kernel
  SDK cross-compiled objects and pinned primary-source hashes are retained.

Independent read-only review found no additional concrete defect in client
scope/ref/close handling or the display parser, clock selection and scoped
capture lifetime. Those reviews do not establish hardware behavior.

## Combined native build

`build/macos-client-display-integration/build-report.json` records the complete
client/model-input snapshot. All 187 source-file hashes and the executable hash
were independently verified against the working tree and copied artifacts.

| Item | Result |
| --- | --- |
| Translation units | 85, including generated pinned firmware |
| Kernel imports | 406 resolved; zero unresolved |
| Build host | Intel macOS 15.8 / 24H22, Apple Clang 17, SDK 26.2 |
| Source snapshot | 3,885,501 bytes |
| Source archive SHA-256 | `b834ed9b132fd2eade2040f0439d80d90d91c13d5f8320896f148832925f2f46` |
| Executable | 2,952,688 bytes |
| Executable SHA-256 | `a81edc4d99a790b2477ce2728588760ae05a42880972325da90ca833324a6d61` |
| Mach-O UUID | `D37C82E4-0B8A-3000-BEDD-E7D81FEC3BB2` |
| Report SHA-256 | `e642e8734bba20e577e491467c15fae913289c1992eee3a20c19ec0fc6218021` |
| Temporary Mac stage | 8,244 KiB; removed |
| Target/build-host load | None |

The no-load diagnostic reports build-folder ownership errors (501:20 versus
root); SDK libkmod deployment warnings are also retained. Import resolution
does not turn those diagnostics into target-load success.

Fresh targeted CMake builds passed all six selected suites: queue, DAL SMU,
DPM, display model input, native ABI and retained-result budget. CMake now
configures 64 suites; this was not a new full-suite run. The copied test log
and independent verification record are alongside the native build report.

The subsequent activation audit identified and fixed a startup ordering issue:
the engine previously checked the inherited display only after audio/DMCUB
control writes. New read-only display and EFI-console preflight checks reject
unsupported input before those controls. A positive preflight grants no
authority; the independent, mandatory post-DMCUB captures still supply the
reservation inventory. Preflight failures use unpublished cleanup.

The patch also preserves failed pipe, raw register value, read validity and
partial power observations through bootdisplaystop and LastEngineObservation.
Attempt bits distinguish absent checks from zero-valued success enums. Actual
CP/shader completion and comparison counts survive a later DisplayBoot failure
without converting it to whole-start success. Entry logs precede control phases.

`build/native-engine-preflight/report.json` records five real Apple Kernel SDK
objects, 7,572,558 ASan/UBSan display-retirement checks and 85 console checks,
all passing. Its 1,808 KiB stage was removed. The eight-source manifest is
`build/engine-preflight-report.json`; independent review verified all hashes
and found no concrete defect. Engine/Lifecycle observation PODs are 592/632
bytes and the fixed registry output is bounded to 118 keys. This patch grants
no new hardware authority and does not relax cold or reset-retirement gates.
The first combined snapshot above predates it; the final combined build is
recorded separately below.

## Final combined build with startup preflight

`build/macos-client-preflight-final/build-report.json` includes that reviewed
patch. All 85 translation units compile and all 406 kernel imports resolve.
All 187 recorded source files, the source archive and executable hashes were
independently verified. The unique 8,276 KiB Mac stage was removed. No target
or build-host load occurred. The ownership/deployment diagnostics described
above remain in this build's log; no loader success is inferred.

| Item | Final snapshot |
| --- | --- |
| Source snapshot | 3,897,435 bytes |
| Source archive SHA-256 | `9415ae3892598a931c185584a9170e2aacfcb166fc49b3b4cc740ad7a207b0ee` |
| Executable | 2,962,304 bytes |
| Executable SHA-256 | `4f7b7ec5166190d83eb526625b99d363491a6ca717e2c908d9ee540cd82a5ffd` |
| Mach-O UUID | `F2363CC2-E023-3B32-8446-017ADFDC9FD0` |
| Build report SHA-256 | `e0659f05def5eef42815bb50da375b6d873fd1eb7d64d86018d2766f00b31397` |

The six unchanged targeted integration suites retain the prior passing result;
the modified preflight/retirement path has the focused sanitizer/native checks
above. No redundant full-suite result is claimed. Use the final snapshot for
current development evidence, preserving the preceding build as history.

## Next implementation dependencies

The activation audit confirms that the missing raw DOMAIN0 power value is
still needed to explain the earlier target rejection. It also identifies
unproved post-reset media/display retirement and an unaudited AMDSupport
co-client. The observed `.Display_boot` is a software boot framebuffer in
Apple's public source, not by itself evidence of a competing modeset writer.
Its complete reserved console extent still matters. Engine opt-in remains
an experimental one-attempt path with no resume/visible-output guarantee.

[Clock-owner design](display-clock-hold-design.md) specifies DAL UCLK
fulfillment and the separate PPSMC SOC/Fabric/metrics path needed before
consuming the DML plan. It records unresolved UCLK return units and the
existing zero-frequency DAL withdrawal claim; neither is proven policy.
[Compiler integration design](compute-compiler-integration.md) specifies the
smallest real next ACO execution test using the exact Apple add-one artifact,
private descriptors/push constants, wave32 dispatch and existing VM retirement.
This would test the compiler-to-HackyNavi bridge; Metal registration remains
separate unfinished work.

Native compilation is not target loading. The immutable
`HackyNavi-live-capture-1e4040229efa` package and its HANDOFF.md remain the
prepared next capture test; it does not contain or activate these engine/client
changes. Never substitute a mutable development bundle for that package.
