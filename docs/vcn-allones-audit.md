# VCN PGFSM all-ones source audit

Reviewed 2026-10-11. Scope: source and supplied evidence only. This audit adds
no hardware reads, source changes, startup admission, SMU commands, or native
builds. The current test package is unaffected.

The failing address is consistent with the actual discovery table and pinned
AMD register definitions. The missing observation is `UVD_POWER_STATUS`, which
Linux reads before deciding whether to read the other VCN registers. A gated
media domain is a plausible explanation for the PGFSM result, but neither
capture proves it. Do not change an all-ones result into a powered-off or
stopped-engine result.

## Actual observations

These two separate supplied captures have the same `ColdAuxiliaryEngines`
result: `DeviceUnavailable`, `Reads=2`, `FailedInstance=0`,
`LastRegister=129028` (`0x1f804`), `LastValue=-1`, `PoweredOffMask=0`, and
`OwnershipStatus=DiagnosticOnlyExternalExclusionNotEstablished`.

| Evidence file | SHA-256 |
| --- | --- |
| `E:/HackyNaviLogs/live-6a3a8173c6e2-20261011-000226-708/device.plist` | `88832fceb84e126ff7315d1084f2f72f10cc8c786f7a24a89b6c2a96ef8456fd` |
| `E:/HackyNavi-diagnostics-20261010-212355/device.plist` | `de54aa03cbad9ee8b5807eafdb6f214c443c813bc7b1f2244a242a3898708642` |

The register read is 32 bits; the serialized signed `-1` means low32
`0xffffffff`. Repeated copies of a property within one plist are not additional
hardware samples. `Reads=2` consists of the SOS check and the first VCN0 PGFSM
read; the checker exits before POWER_STATUS, VCN1, JPEG, or LSDMA are observed.
`PoweredOffMask=0` is an unconfirmed result, not proof that every domain is on.

## Address, version and instance checks

All upstream references below use Linux commit
`3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0` from `.research/sources.json`.
The discovery table reports two unharvested UVD/VCN entries, hardware ID 12:

| Instance | Major/minor/revision; variant | Raw base[0] | Raw base[1] |
| --- | --- | --- | --- |
| 0 | 4.0.0; 0 | `0x7800` | `0x7e00` |
| 1 | 4.0.1; 1 | `0x7b00` | `0x12000` |

The generated header assigns PGFSM_CONFIG offset 0, PGFSM_STATUS offset 1,
and POWER_STATUS offset 2, all with BASE_IDX 1. Linux register addressing adds
the selected discovered base to the DWORD offset. The native adapter indexes
its mapped DWORD array with the resulting byte offset divided by four.
Sources: [generated VCN4 offsets, lines 1145–1151](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/include/asic_reg/vcn/vcn_4_0_0_offset.h#L1145-L1151),
[SOC15 offset calculation, line 36](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu/soc15_common.h#L36).

| Register | VCN0 BAR5 byte offset | VCN1 BAR5 byte offset |
| --- | --- | --- |
| UVD_PGFSM_CONFIG | `0x1f800` | `0x48000` |
| UVD_PGFSM_STATUS | `0x1f804` | `0x48004` |
| UVD_POWER_STATUS | `0x1f808` | `0x48008` |

For the failing read, `(0x7e00 + 1) * 4 = 0x1f804`. Linux retains each
discovery entry's bases by instance and selects the VCN implementation from
instance 0's version. Its 4.0.0 selection uses `vcn_v4_0_ip_block`, whose
multi-instance accesses use `vcn_4_0_0` definitions. The instance-1 revision
therefore does not establish that the failing instance-0 register uses a
different header. Sources: [discovery base storage, lines 1889–1920](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c#L1889-L1920),
[VCN selection, lines 2893–2937](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c#L2893-L2937).

No source-backed address correction was found. This verifies the calculation,
not the availability of that register window under the inherited firmware.

## Linux's read and power prerequisites

Linux's VCN4 dump list places POWER_STATUS first and includes PGFSM_CONFIG and
PGFSM_STATUS later. `amdgpu_vcn_dump_ip_state` explicitly says
“mmUVD_POWER_STATUS is always readable and is the first in reg_list.” It reads
that register, checks `UVD_POWER_STATUS_TILES_OFF`, and only reads the remaining
registers when that bit is clear. The constant is `0x1`, so this particular
dump decision uses bit 0. Sources: [VCN4 dump list, lines 56–89](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu/vcn_v4_0.c#L56-L89),
[dump prerequisite, lines 1592–1618](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu/amdgpu_vcn.c#L1592-L1618),
[constant, line 264](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu/amdgpu_vcn.h#L264).

The ordinary VCN4 start path also calls `amdgpu_dpm_enable_vcn(true, instance)`
when DPM is enabled, before static power-gating configuration and PGFSM polling.
The SMU13 operation sends PowerUpVcn with `instance << 16`. This supports a
power-access precondition, not an unconditional claim that every Linux read
follows a power command. Sources: [VCN start, lines 1136–1157](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu/vcn_v4_0.c#L1136-L1157),
[static power setup, lines 617–674](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu/vcn_v4_0.c#L617-L674),
[SMU13 operation, lines 1857–1871](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0.c#L1857-L1871).

Linux's dump comment is strong evidence for the smallest next read. It is not
independent validation that this board's pre-PSP macOS firmware state exposes
the register. POWER_STATUS is also a software-written indicator: Linux sets
TILES_OFF before requesting static power-off, then separately polls PGFSM for
completion. Its bit is therefore not a standalone physical completion proof.
See [static power-off, lines 690–727](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu/vcn_v4_0.c#L690-L727).
Even a readable tile-off value proves neither retirement of queued DMA nor
exclusion against future firmware-driven power transitions.

## Current implementation and smallest next diagnostic

`src/cold_aux.cpp:122` reads PGFSM_STATUS first. POWER_STATUS already exists as
`Layout::vcn[instance][0]` (`vcnFields` offset 2), but is reached only after
PGFSM classification succeeds. `HackyNaviDevice.cpp:520` invokes this checker
after the posted-state observation. Its auxiliary read allowlist already
contains both POWER_STATUS addresses (`:287–295`); the actual volatile read
is at `:329`. This is a real read-only diagnostic, with no SMU power-up call.

The minimal new evidence is a bounded, per-instance POWER_STATUS inventory
using the existing discovered layout and native capture gate. Preserve the
attempted register, raw value, successful-read flag and instance independently
of the full stopped-engine result. At most two samples per instance can expose
an observed change; equal samples do not establish permanent stability.
Do not add broad MMIO scans or mailbox writes.

| New observation | Interpretation and next action |
| --- | --- |
| Transport read fails or value is `0xffffffff` | Register remains unavailable. Do not decode its power bits or probe further client registers. Investigate BAR/provider/power lifetime before interpreting a domain state. |
| Valid POWER_STATUS has bit 0 set | Linux's dump would omit PGFSM and other VCN registers. Record that access prerequisite; leave stopped-engine status unproved and omit further gated-window probes. |
| Valid POWER_STATUS has bit 0 clear | Linux's dump permits subsequent VCN reads. Under an appropriate live owner, capture status around a bounded PGFSM observation. Repeated all ones still fail; they require further firmware/decode investigation. |
| Before/after values differ | Record a changing observation and stop classification. |

This diagnostic must not replace the admission checker with a POWER_STATUS
test. Preserve the historical `DeviceUnavailable` evidence. Any future
decision to omit the checker because registers are unavailable must publish
that unperformed check explicitly, never return success or a stop certificate.

The portable `early_media::prepare` (`src/early_media.cpp:180–202`) does contain
SMU version queries and VCN0/VCN1/JPEG power-up messages before the full checker.
It is not called by this native capture. Its header (`early_media.hpp:81–94`)
explicitly leaves the native pre-PSP adapter unimplemented pending real memory
preservation and host DMA exclusion. Connecting that path solely to make a
diagnostic read succeed is not justified. Current first-attach engine code
instead uses the separate LSDMA check (`HackyNaviEngineController.cpp:271–275`);
this VCN diagnostic failure is not established as its sole startup blocker.

## Verification boundary

Both plists were parsed directly, and the generated offset header,
`amdgpu_vcn.h`, `vcn_v4_0.c`, and `amdgpu_vcn.c` were byte-compared with their
pinned upstream raw files. All four matched. No target test was run, and the
POWER_STATUS values remain unknown. This audit changes only this document.
