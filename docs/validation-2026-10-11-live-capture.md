# RX 7900 XTX live-capture boot evidence

The target loaded the frozen `HackyNavi-live-capture-6a3a8173c6e2` build on
macOS 15.8.1 / 24H32. This proves the diagnostic capture path ran on the board;
it does not prove GPU execution, controlled scanout, or Metal.

## Preserved evidence and artifact identity

The original folder is
`E:/HackyNaviLogs/live-6a3a8173c6e2-20261011-000226-708`. The OpenCore session
preserved all 18 files at
`C:/Users/Ras/Documents/ChatGPT/Hackintosh/reports/hackynavi-live-capture-6a3a8173c6e2/collected/live-6a3a8173c6e2-20261011-000226-708`.
Root independently matched every file to `collected-manifest.json`, with no
missing or extra files. Originals were not changed. The folder label is used
as an evidence identifier; target log clock values are not assumed accurate.

The selected package executable SHA-256 is
`6a3a8173c6e25e10a6ed13aadd9690d6d1a2d0bf13cf2a22f465508fa23f1a4e`.
The loaded Mach-O UUID matches `EA44A013-8203-36C3-84E1-523140BA82C9`.
UUID matching associates the observed load with the selected artifact; it is
not a runtime memory hash. `device.plist` SHA-256 is
`88832fceb84e126ff7315d1084f2f72f10cc8c786f7a24a89b6c2a96ef8456fd`.

`scripts/analyze-macos-diagnostics.py --mode live-capture` against that exact
bundle passed, with output in
`build/validation/live-capture-6a3a8173c6e2-20261011.json`.
All twelve collector commands returned exit 0. The user separately reported
HDMI was connected and no sleep, reboot, hang, or panic occurred before
collection (reply: "HDMI, and no nothing happened"). This is not a stress,
suspend/resume, frame-presentation, or driver-controlled display test.

## Observed results

| Observation | Actual evidence |
| --- | --- |
| Matching hardware | One passive observer device, PCI `1002:744c`, revision `c8`, board `148c:2422`. |
| Explicit capture opt-in | Effective boot arguments contain `-hackynavi-device` and `-radvesa`. |
| Current capture | `HackyNaviDevice`, `SnapshotStatus=ValidatedPowerUpCapture`, `CaptureStage=Complete`, `SnapshotValid=true`; capture/snapshot/power generations all 3. |
| IP discovery | Acquisition, inventory and register layout validated; 61 IP entries. |
| Runtime ROM | 57,856 bytes acquired and retained for this power generation; PCI/subsystem binding, firmware-info and VRAM-usage decoding succeeded. |
| Memory | Validated register snapshot; 25,753,026,560 reported VRAM bytes, 256 MiB CPU-visible BAR0 aperture; reservations remain unestablished. |
| Posted state | SOS zero and BIOS scratch accepted by the posted-cold observation. This does not establish external exclusion. |
| Boot display | Rejected on pipe 0 at DOMAIN0_PG_STATUS, BAR5 byte offset `0xd504`; classification incomplete, no usable reservation. |
| Auxiliary engines | Second read returned `0xffffffff` at VCN0 UVD_PGFSM_STATUS, BAR5 byte offset `0x1f804`; `DeviceUnavailable`. |
| Capabilities | `AccelerationAvailable=false`, `DisplayInitializationAvailable=false`. |

IORegistry serializes the unsigned all-ones auxiliary value as integer `-1`;
its low 32-bit register value is `0xffffffff`. Neither nested failure is
overridden by the outer capture's `Complete` status.

## Consequences for implementation

The display's two attempted reads were the SOS check followed by DOMAIN0
status. Its discovery-derived address agrees with the pinned DCN3.2 header:
`(DMU base[2] 0x34c0 + offset 0x81) * 4 = 0xd504`. The old capture omitted the
raw rejected value, so it cannot distinguish a transition from an unsupported
bit pattern. No DOMAIN0 config or HUBP-window read occurred. The follow-up
code retains the last raw read and validity plus each power-probe field, with
no added MMIO and no relaxed acceptance rule.

The auxiliary address also agrees with the real discovery and pinned header:
`(VCN0 base[1] 0x7e00 + offset 1) * 4 = 0x1f804`. An offset correction is not
supported. Linux powers the VCN domain through SMU before querying its PGFSM;
a gated domain is a possible explanation, not a fact established by all ones.
Early SMU media-domain ownership and startup sequencing must be implemented
and checked before relying on this stopped-engine predicate. All ones remain
a failure, and powering a domain is not proof that its engines are stopped.

This test grants no allocator, DMA, GPU/audio reset, or modeset authority.
External exclusion, engine startup, retirement/recovery, framebuffer/IOSurface,
Metal, HDMI FRL/DSC and DP output remain under development. The original test
package is immutable; follow-up source is not retroactively part of this boot.

The follow-up is now frozen as `HackyNavi-live-capture-1e4040229efa`, UUID
`8DE5BBB4-D07A-3965-B970-3CCE28B7ECAA`, with its own package handoff. It captures
the missing values only; no power-domain transition was added to this test.
The default collector output must remain a new folder so the two boots are
never mixed. The expected next observation is the actual DOMAIN0 status and
validity bits, even if the top-level nested display result still rejects it.
