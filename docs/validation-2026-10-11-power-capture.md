# Follow-up target display-power capture

Reviewed 2026-10-11. Evidence supplied by the user at
`E:/HackyNavi-diagnostics-20261010-212355` was copied byte-for-byte to
`build/validation/live-capture-1e4040229efa-20261010-212355/evidence`.
All 14 files (392,283 bytes) were preserved and SHA-256 recorded in
`collected-manifest.json`. The original files were not modified. The folder's
timestamp is an evidence identifier, not an assumed accurate wall clock.

The diagnostic analyzer against the explicit frozen
`build/releases/HackyNavi-live-capture-1e4040229efa/HackyNavi.kext` passed;
`analysis.json` records the exact observations and evidence hashes. All twelve
collector commands returned exit 0. The loaded UUID is
`8DE5BBB4-D07A-3965-B970-3CCE28B7ECAA`, matching that package. Its executable
SHA-256 is `1e4040229efacc1c5e060cfdc5ef4b9ee453c7e7339154bb39f1fe473c0a097a`.
UUID association is not a runtime memory hash. Immutable package metadata
describes validation at packaging time; this document records the later boot.

## Observations

- Target: macOS 15.8.1 / 24H32, x86_64 Darwin 24.6.0, RX 7900 XTX
  `1002:744c` revision `c8`, subsystem `148c:2422`.
- Effective arguments include `-hackynavi-device` and `-radvesa`, with no
  `-hackynavi-engines`.
- Capture completed with a valid snapshot and matching capture, snapshot and
  power generation 3. Discovery validated 61 IP entries; the live 57,856-byte
  ROM matched the board. Memory registers report 25,753,026,560 VRAM bytes and
  a 256 MiB CPU-visible aperture. Allocator reservations are not established.
- The formerly omitted `DOMAIN0_PG_STATUS` value at BAR5 byte `0xd504` is
  **`0x10000000`**. Its read succeeded. `LastPowerProbe.ReadsSucceededMask=1`
  means only `StatusBefore` was observed; the zero `Config` and `StatusAfter`
  values are unobserved placeholders, not hardware values.
- The current classifier rejects this status before reading DOMAIN0 config
  or any HUBP window. `CompleteClassification=false`, no planes/reservations.
- VCN0 `UVD_PGFSM_STATUS` at byte `0x1f804` again returned low32
  `0xffffffff`; IORegistry plist stores the all-ones value as signed `-1`.
  This remains `DeviceUnavailable`, independently of outer capture success.
- The service tree still contains the inherited `IONDRVFramebuffer` display.
  Acceleration and display initialization are explicitly false. These logs
  do not demonstrate HackyNavi engine submission, controlled scanout or Metal.

## Meaning of the new value

The pinned AMD DCN3.2 definitions place `DOMAIN_PGFSM_PWR_STATUS` in bits31:30
and `DOMAIN_DESIRED_PWR_STATE` in bit28. This observation therefore has current
FSM value 0 and desired-state bit 1. Linux's `dcn32_hubp_pg_control` waits for
FSM 0 after requesting power-on and FSM 2 after requesting power-off. Its
separate `dcn32_enable_power_gating_plane` changes the config's FORCEON bit.
The missing config prevents distinguishing those controls here. This single
read does not justify declaring the domain fully off, a completed transition,
or permanent ownership of a readable HUBP window.

Sources: pinned Linux `3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`, local
`.research/linux/dcn_3_2_0_sh_mask.h:2774-2783` and
`.research/linux/dcn32_hwseq.c:dcn32_hubp_pg_control` /
`dcn32_enable_power_gating_plane`. Current implementation:
`src/boot_display.cpp:readPower`. The source audit is documented in
[the FORCEON audit](boot-display-forceon-audit.md). A narrow correction now
requires the actual config to equal `0x1` (FORCEON1/GATE0) before admitting
`0x10000000`, plus the existing repeated status/config stability checks.
CONFIG0, CONFIG0x101, transitions, unknown encodings and all ones remain
rejected. This is conditional on fresh actual reads; these supplied logs
still do not establish that the board's CONFIG was `0x1`.

An independent inventory now preserves STATUS/CONFIG/STATUS for all four
always-on power controls, at most twelve reads, stopping on transport failure
or all ones. No power register is written. When the ordinary two register
passes complete and pass stability checks, `SnapshotReadComplete` separately
permits diagnostic publication of the first raw snapshot even if later plane
decoding fails. Powered-off HUBP windows are omitted. This avoids losing the
next format/addressing observation while keeping unobserved data distinct.
Native validation and the follow-up artifact are recorded below.

The user's earlier HDMI/no-hang response belongs to the previous capture.
This submission contains no separate user report about this boot's connector,
desktop responsiveness or intervening sleep. No new hardware test is inferred
from that earlier answer.

## Validated correction and next test

`build/native-display-forceon-complete` ran the focused display suite with
native macOS ASan/UBSan: **5,033,482 assertions, zero failures**. The current
device and portable display source also compiled as actual x86_64 kernel
objects. All captured source hashes matched after export; the 1,632 KiB
temporary stage was removed. Independent review of the reporting and power
logic found no defect; see `build/display-capture-final-independent-review.json`.
The separate consumer regression for `boot_display_stop` passed 22,512,972
local sanitizer assertions; no production stop change was needed. These
software tests are not target hardware execution.

The follow-up artifact is
`build/releases/HackyNavi-live-capture-dbbcb5ec0ba9.zip` (2,219,830 bytes).
Its [complete OpenCore handoff and collection procedure](../build/releases/HackyNavi-live-capture-dbbcb5ec0ba9/HANDOFF.md)
is the procedure for the next boot. All 15 package files were verified in the
ZIP. This exact executable has not yet been loaded on the target.

- Executable SHA-256: `dbbcb5ec0ba932a667d3fe295baf59939753246c4de983e316889263f1f9997c`.
- Mach-O UUID: `49B982AA-6376-3D09-B101-CB5B6B478A6E`.
- ZIP SHA-256: `0305b105218206588f51d1c206e91758d61d2a85368ddcbfc5d392789e24ac9a`.
- Source archive SHA-256: `0dc172017de5e82f5b8e055a7fdcdcf6bbd1933dbad16c704cb79371453e04f5`.
- Native build report SHA-256: `f5e33fd1d48dcf6c6b750de941e038faca342c296b8ec641e2ccb50bf30cb234`.

`build/macos-live-forceon-final` compiled and linked 74 translation units;
all 353 imported symbols resolved on the build Mac. Its 167-file source
manifest and archived artifact match. The 7,816 KiB remote stage was removed.
The no-load diagnostic exited 0 but reported build-folder ownership 501:20
instead of 0:0; the SDK's libkmod objects also warn about deployment newer than
15.0. Those diagnostics are preserved in the package and do not prove loading.

The source snapshot is derived from the already tested `1e4040229efa` package,
with exactly three changed files: `boot_display.hpp`, `boot_display.cpp`, and
only `captureBootDisplay` within `HackyNaviDevice.cpp`. Every byte outside that
function in the device source matches the prior snapshot. All remaining
sources, including the 74-unit build list, are unchanged. The later development
VM/product changes are not in this test. Selection verification is retained in
`build/macos-live-forceon-final/source-selection-validation.json`.

Boot with the same HDMI setup, retain `-hackynavi-device` and existing
`-radvesa`, leave `-hackynavi-engines` absent, and collect a fresh complete folder
with the package's collector. The expected new evidence is all four valid
power probes and, when both register passes are stable, the raw display
snapshot. A CONFIG other than the admitted value can still stop classification;
that is useful evidence, not permission to force the GPU on. Do not initiate
sleep, hot-plugging or GPU stress as part of this capture. The session did not
modify OpenCore or reboot the machine.

The VCN all-ones result remains unresolved. The separate
[read-order audit](vcn-allones-audit.md) confirms the PGFSM address/version and
identifies a missing earlier POWER_STATUS observation. Linux checks that
access prerequisite before dumping the other VCN registers; it is not a
physical power-off or stopped-DMA certificate. That additional diagnostic
is not included in this frozen display-capture package. Later development predicates in
`cold_dma`, `display_idle` and `display_power` also require separate
source-backed reconciliation before engine testing. This correction neither
completes engine startup nor provides Metal or controlled display output.
