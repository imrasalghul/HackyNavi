# Target capture: current display power state and CONFIG0

Reviewed 2026-10-11. The user's `E:/HackyNavi-diagnostics-20261010-232322`
submission was preserved byte-for-byte in
`build/validation/live-capture-dbbcb5ec0ba9-20261010-232322/evidence`.
Its manifest records all 14 files, 395,564 bytes, and SHA-256 hashes. The
originals were not modified. The folder timestamp identifies the evidence;
it is not assumed to be an accurate wall clock.

The analyzer passed against the explicit frozen `dbbcb5ec0ba9` bundle. All
twelve collector commands returned zero. macOS is 15.8.1 / 24H32. Loaded UUID
`49B982AA-6376-3D09-B101-CB5B6B478A6E` matches that bundle, whose executable
SHA-256 is `dbbcb5ec0ba932a667d3fe295baf59939753246c4de983e316889263f1f9997c`.
UUID association does not hash runtime kernel memory. This later boot record
supersedes the earlier document's statement that this package was untested;
its immutable package metadata remains unchanged.

## What the board reported

All four independent display power probes completed successfully. Every pipe
reported STATUS-before `0x10000000`, CONFIG `0`, STATUS-after `0x10000000`,
with validity mask 7. The inventory completed twelve reads with status `ok`.
Thus FORCEON is clear on this board, as is POWER_GATE. The prior hypothesis
that FORCEON explained the observed status is disproved by these fresh reads.

The tested classifier nevertheless stopped at pipe0 CONFIG byte `0xd500`:
the old predicate required CONFIG1 for this status. Its separate last-probe
mask is 3, so that probe's confirming status was not read. No raw display
snapshot, complete plane inventory or allocator reservation was published.
The independent four-pipe inventory is evidence only, not hardware ownership.

VCN0's PGFSM register at byte `0x1f804` again returned all ones. Auxiliary
capture remains `DeviceUnavailable`, with no powered-off proof. Outer capture
completion does not change that result. These logs demonstrate neither engine
execution, HackyNavi-controlled scanout nor Metal. The earlier user's HDMI and
responsive-desktop answer concerned a different boot; this submission contains
no new separate report about cable, responsiveness or intervening sleep.

## Source-based correction

DCN3.2 defines current PGFSM status in bits31:30 and desired state in bit28.
Linux requests power-on by clearing POWER_GATE and waits for current FSM0;
it does not require the desired bit to clear. See the pinned
[Linux DCN3.2 implementation](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/hwss/dcn32/dcn32_hwseq.c).
The corrected capture admits raw STATUS0 or `0x10000000` with CONFIG0 or1,
but still requires stable full raw observations and rejects GATE, unknown
bits, changing state and failed/all-ones reads. This grants no persistent
ownership or permission to skip later checks.

The new auxiliary inventory reads each VCN POWER_STATUS twice before any
broader classifier access. It stops on transport failure, all ones or change;
all observations carry attempted, successful and valid masks. An observed
TilesOff indication does not prove physical power-off or stopped DMA. If
both instances do not establish the bounded read prerequisite, the broader
classifier is explicitly skipped. This follows the ordering found in the
[Linux VCN dump path](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu/amdgpu_vcn.c).
No power register write is added by this inventory.

## Validation and follow-up

`build/native-power-vcn-final` passed native macOS ASan/UBSan suites:
6,592,514 capture assertions, 37,457,089 display-retirement checks and 63,258
auxiliary checks, all with zero failures. The current Device, boot-display
and cold-auxiliary sources compiled into actual x86_64 kernel objects.
All staged source hashes matched after export; its 3,196 KiB temporary mini
stage was removed. These checks do not establish hardware execution.

The next capture build was generated from the previously loaded
`dbbcb5ec0ba9` source snapshot with exactly five selected changes: the
boot-display header/source, only the new power-inventory declarations and
implementation in the cold-auxiliary header/source, and only `captureColdState`
in Device. All other bytes remain from the loaded base. Selection evidence
is in `build/power-vcn-source-preflight/source-selection-validation.json`.
The later product transport and engine power-owner changes are outside that
capture snapshot.

The native link passed: 74 translation units, 353 resolved imports, all 167
source files verified. The 7,824 KiB full-build stage was removed. No-load
diagnostics exited zero but retained uninstalled-folder ownership 501:20
errors; SDK 26.2 libkmod deployment warnings also remain in the build log.
Neither result establishes compatibility on the target until it boots.

The frozen follow-up is
[`HackyNavi-live-capture-45eab551a719.zip`](../build/releases/HackyNavi-live-capture-45eab551a719.zip),
2,223,886 bytes. All 15 package files were verified against the ZIP.

- ZIP SHA-256: `ec218369d0e2578433531e50c597c29ebf393cc67a849e2710e91d64fb732d3e`.
- Executable SHA-256: `45eab551a71944458830b5e3c7b7d44142c65fb4ba641f6dfe922c9a6e22f9f9`.
- Mach-O UUID: `564B24EE-00F5-35C0-8071-38310097F55A`.
- Source archive SHA-256: `215169e02e0f0539a91938797cdb350499db7d6f0a7e8f7321b1c1392fe4d72e`.
- Native build report SHA-256: `1e43ad1ff297c52d87d0f766aab2d284d5c8a77f8d5a94f5265d3289c08574f6`.

Use the [packaged OpenCore handoff](../build/releases/HackyNavi-live-capture-45eab551a719/HANDOFF.md).
Keep the same display setup, `-hackynavi-device` and existing `-radvesa`, with
`-hackynavi-engines` absent. The requested evidence is the full collector
folder, fresh loaded UUID, raw display snapshot if its passes finish, and the
new VCN inventory with validity masks and classifier-attempt status. Capture
failure remains useful evidence. Do not add sleep, stress or engine activation
to this capture. No media was modified and no reboot was initiated here.

## Current engine startup audit

The current development engine is distinct from the diagnostic binary above.
The supplied boot arguments contain `-hackynavi-device` and omit
`-hackynavi-engines`; neither an engine attempt nor a GPU verification result
appears in this capture. The observed CONFIG0/current-ON rejection is corrected
in the current production boot-display capture and its cold-DMA, idle, power,
and scanout-retirement consumers. That source correction is not a fresh
hardware result.

`HackyNaviEngineController::start` first reacquires discovery, ROM, memory, and
posted-state observations under its actual startup Operation. The first
subsequent hardware prerequisite for which this folder has no evidence is
`checkTrainingIdle`: two exact PSP bootloader-command reads of `0x80000000`,
bracketed by zero sOS observations. The pinned `psp_v13_0_wait_for_bootloader`
also waits for that exact value; there is no source basis to waive it. The next
unmeasured prerequisite is the distinct LSDMA stopped check. The old VCN all-ones
result cannot be substituted for either check: current firmware-first engine
startup uses the separate LSDMA API and does not claim VCN/JPEG were drained.

The display classifier in these logs stopped after its third read, before any
HUBP surface, address, VMID, pitch, or tiling inventory. Therefore these files
cannot establish that the corrected full display preflight will pass, or
identify its next failing register. Console capture, inherited DMCUB stopping,
CP/SDMA quiescence, cold-DMA checks, complete reservations, and every later
firmware/queue verification likewise require new actual observations.

The next engine test must retain the exact loaded UUID and
`LastEngineObservation`/engine stage and register results. An earlier corrected
read-only capture can establish the newly reachable display inputs, but cannot
authorize engine DMA or certify those later checks. No gate was weakened or
invented register value introduced during this audit. Target boot/media changes
remain a separate test handoff; this source audit made none.
