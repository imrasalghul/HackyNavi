# DCN3.2 current power state in production consumers

Reviewed and implemented 2026-10-11. The supplied capture
`E:/HackyNavi-diagnostics-20261010-232322/device.plist` reports all four
DOMAIN0..3 observations as STATUS-before `0x10000000`, CONFIG `0`,
STATUS-after `0x10000000`, with all three reads successful. This is observed
register data, not proof of idle display clients or continuing ownership.

## Source interpretation

At pinned Linux commit `3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`,
`dcn32_hubp_pg_control` writes the GATE request and polls the **current**
`DOMAIN_PGFSM_PWR_STATUS` field for 0 when powering on and 2 when powering off.
It does not include `DOMAIN_DESIRED_PWR_STATE` in that completion predicate.
The generated register definitions put current state in bits 31:30 and
desired state in bit 28; CONFIG contains FORCEON in bit 0 and GATE in bit 8.
`dcn32_enable_power_gating_plane` sets FORCEON to disable gating. Sources:
[sequencer](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/hwss/dcn32/dcn32_hwseq.c#L137-L196),
[generated fields](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_2_0_sh_mask.h#L2774-L2813).

## Implemented predicates

| Current state | Complete permitted raw STATUS | Complete permitted CONFIG |
| --- | --- | --- |
| ON, PGFSM=0 | `0`, `0x10000000` | `0`, `1` |
| OFF, PGFSM=2 | `0x80000000`, `0x90000000` | `0x100` |

Reserved bits, intermediate current states in stable admission, explicit gate
requests against ON, FORCEON against OFF, and FORCEON+GATE all reject. The
desired bit is retained in every stability comparison. Accepting either value
at first observation does not permit it to change during a stable check.

`cold_dma.cpp` now preserves raw status and configuration through all power
observations, including both passes and the final checks. It rejects reserved
bits rather than masking them away. The existing IH, writeback, MALL, SubVP,
VM-page trigger and cold firmware checks remain required. Only an observed
OFF domain skips its gated client registers.

`display_idle.cpp` classifies OFF from the current state instead of treating
every nonzero raw STATUS as OFF. Therefore the supplied `0x10000000` encoding
still requires the actual HUBP blank/drain bits, as well as OTG and MPC checks.
Both post-pipe and final-pass raw status/configuration comparisons remain.

`display_power.cpp` applies current-state classification to admission,
HUBP/DPP idle checks, already-on adoption, power-off admission and transition
completion. Polling retains the first completed raw sample; both subsequent
verification stages compare that exact value and the commanded CONFIG. A
desired-bit change after completion is a fault requiring retention. Existing
bounded polling, IP_REQUEST_EN sequencing, request restoration, actual clock
checks and ambiguous-write retention remain in effect.

The controller still requires the real caller's exclusive PCI/PM/reset,
firmware and display-route ownership. No field alone grants that ownership,
global DMA retirement, a memory release, or acceleration.

## Verification and scope

Strict C++11 tests and separate UndefinedBehaviorSanitizer builds passed:

| Suite | Checks | Failures |
| --- | ---: | ---: |
| Cold DMA | 1,146,247 | 0 |
| Display idle | 328,847 | 0 |
| Display power | 1,353,761 | 0 |

The register models cover both ON and OFF encodings, both ON configurations,
all four pipes, mixed OFF masks, reserved bits, request conflicts, every
transport/all-ones failure, raw desired/configuration changes, live drain
rejections, transition completion in both directions, and failed writes both
accepted and rejected by the modeled device. These are host tests; no GPU was
accessed and no native target transition was performed. Hash-bound evidence
is in `build/display-power-current-state-validation.json`.

No headers, native owner methods or diagnostic package files changed in this
work. The separately frozen boot-display capture/stop implementation still
accepts the narrower raw `0x90000000` OFF form; it fails closed on `0x80000000`.
Reconcile that separate consumer after its frozen diagnostic build, without
rewriting the historical capture or claiming this change is target-tested.
