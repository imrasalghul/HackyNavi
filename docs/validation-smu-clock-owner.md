# PPSMC clock and metrics integration, 2026-10-11

The existing native FirmwareController now owns the PPSMC SOC/Fabric companion
and its private VRAM metrics allocation. This is compiled driver code; no target
GPU invocation, clock change or metrics DMA has been observed. The immutable
`1e4040229efa` capture ZIP is unchanged and does not contain this integration.

## Implemented behavior

`src/smu_clock.cpp` borrows the already initialized SMU state. It rechecks the
actual firmware/interface and running DPM features, queries source-defined
frequency limits, and sends the SOC/Fabric soft maximum followed by minimum.
The maximum request follows the source's MHz+1 rule without colliding with the
automatic-policy marker. An ACK is not a measured or fixed clock guarantee.

Metrics use the exact interface 0x3d external table5 layout: 392 bytes, thirteen
current clocks and the counter at byte80. The native adapter allocates and
zeroes an actual 4096-byte VRAM page, orders the upload, publishes both address
halves, submits the real transfer, waits for its ACK, and performs ordered
uncached MM_DATA readback. The address comes from the live allocator handle.
There is no caller-supplied address, cached success or fabricated data path.

Every access uses the actual Operation or RetirementScope and installed GART
Access. Firmware then GART locks cover the entire protocol. A single monotonic
native deadline begins before lock acquisition and covers all subprotocols,
including HDP accesses. Faults revoke common firmware readiness without
recursively taking or prematurely releasing those locks.

Normal retirement seals the sole table producer only when the exact latest
transfer completed, ordered readback finished and the allocation still matches.
It does not claim the firmware pointer was cleared. Touched SOC/Fabric ranges
can return to Linux's automatic policy, max0xffff then min0, after fresh idle
CP, reset/disabled DMCUB and physical display-idle checks. The same conditions
are rechecked afterward. This does not restore an unknown prior owner's policy
or withdraw DAL/UCLK constraints. Partial or failed transactions remain retained.

Engine teardown now invokes this close after Display, queue, VM and RLC
retirement and before PSP teardown. All allocation-release paths independently
reject outstanding SMU table/range retention, including the older reset path.
Clock failures log their distinct protocol statuses and retention flags.

## Verification

- `build/smu-clock-validation.json`: 61,081 ASan/UBSan checks, actual
  MacKernelSDK object, and independent pinned-header ABI verification.
- `build/native-smu-clock-owner/report.json`: the same 61,081 checks passed
  under native Apple Clang sanitizers; four actual kernel objects compiled.
  The 2,280 KiB stage was removed. The subsequent log-only native change is
  included in the full build below.
- `build/smu-clock-peer-review.json`: independent ownership, scope and
  retirement review, with exact reviewed source hashes.
- Fresh CMake integration built and passed `smu-clock` (1/1). This is not a
  claim that every unrelated suite was rerun.
- `build/macos-smu-clock-owner-final/build-report.json`: native full link of
  88 translation units; all406 imported kernel symbols resolved; all228
  recorded source hashes matched the worktree after export. The 9,524 KiB
  stage was removed. Native/cross source lists agree.

Build host: Intel macOS15.8/24H22, Apple Clang17, SDK26.2, deployment15.0.
No kext was loaded there. The no-load kmutil commands returned zero, but their
build-folder ownership501:20 diagnostics and SDK libkmod deployment warnings
remain in the log. Linking is not target loading or GPU execution evidence.

| Artifact | SHA-256 / identifier |
| --- | --- |
| Source archive | `bd4a9c03d60de8a979b7f351edf0012a861bd98f43a9a02dec6e5e2e933df269` |
| Artifact archive | `157e8fb2fd4aed93b3c14ae6616e6e2ee70963c6f3689d65d7ef014109839c89` |
| Executable | `b1e33d63e223c271d3580d0fbbc5df1a706f3429973040b4ef497665a6b3a429` |
| Mach-O UUID | `ECC2CA6E-F9BA-3EE1-8C20-751015B139E9` |
| Build report | `ad78c5bf9862ca493e4052005dfa18326eb41c01cbae94f24edd23e49392d9a1` |

## Remaining activation boundary

The native request/capture entrypoints and real teardown are linked. No startup
caller yet programs arbitrary ranges or starts metrics capture. The complete
display transaction still needs its trusted DML consumer, coordinated DAL and
PPSMC constraints, recapture, bandwidth/watermarks, timing/link and scanout.
Recovery of an ambiguous SMU table command is being designed separately; MODE1
is not treated as proof of a cleared SMU table pointer. No MTLDevice, IOSurface,
driver-controlled output, HDMI FRL/DP DSC or target shader execution is implied.

Protocol provenance and the explicit synchronous-transfer retirement inference
are in [the owner audit](smu-metrics-native-integration.md) and
[clock hold design](display-clock-hold-design.md).
