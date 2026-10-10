# Owned SOC/Fabric deep-sleep policy

The existing SMU clock owner can now inhibit SOC and Fabric clock deep sleep,
then restore the exact feature bits it observed before the change. This is one
prerequisite for the proposed fixed display-clock profile. It does not yet
authorize that profile, set DF C-state allowance, prove a clock frequency, or
enable scanout.

## Source contract

All Linux references use commit
`3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`:

- [`smu_v13_0_deep_sleep_control`](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0.c#L1908)
  selects the individually supported DS_FCLK and DS_SOCCLK features.
- [`smu_cmn_feature_set_enabled`](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/pm/swsmu/smu_cmn.c#L862)
  maps a feature to its ASIC bit and changes its enabled-mask state. The low
  mask uses EnableSmuFeaturesLow / DisableSmuFeaturesLow.
- The [SMU13.0.0 interface](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu13_driver_if_v13_0_0.h#L60)
  assigns DS_SOCCLK bit 12 and DS_FCLK bit 13. Its
  [message IDs](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu_v13_0_0_ppsmc.h#L46)
  are enable-low `0x08` and disable-low `0x0a`.

The new API has no arbitrary mask input. It changes only the intersection of
the actual enabled mask with `0x3000`; an empty intersection emits no mutation.
No high-half zero command, unrelated deep-sleep feature, or DF_CSTATE feature
bit is written. Interface `0x3d`, the actual firmware version, healthy existing
SMU owner, disabled GFXOFF, and enabled SOC/Fabric DPM remain required.

## Ownership and failure behavior

`inhibitSocFabricDeepSleep` reads the complete enabled mask twice, requires
agreement, privately records the baseline and intended delta before any
mutation, and reads the complete resulting mask twice after the command ACK.
Both results must equal the original mask with just the selected bits cleared.
No externally supplied observation grants authority.

The native Firmware bridge holds the existing FunctionGroup scope and
Firmware/GART locks. Its fresh `clockQuiescent` checks require no queue, VM, or
display owner, idle physical pipes, and disabled/reset DMCUB before and after
the operation. This is an explicit cold-policy API; no new automatic hardware
call is inserted into boot or the diagnostic kext.

While the inhibition is owned, range requests and metrics captures reject a
different observed feature mask. This is sampled readback under the serialized
owner, not a claim of continuous hardware measurement. A timeout, ambiguous
write, bad reply, changed mask, expired scope, or failed native postcondition
faults the existing firmware authority. Private attempted/ACK/readback history
is retained; there is no retry through a fresh controller.

`restoreSocFabricDeepSleepAfterQuiescence` requires the same live owner, a
successful inhibition, and no retained owned SOC/Fabric ranges. It verifies
the held full mask twice, enables only the previously captured bits, and
verifies the original full mask twice. An originally disabled feature remains
disabled. Restoration is terminal and repeat calls do no hardware work.

Native retirement closes the metrics producer, returns owned soft ranges to
their separately documented automatic policy, then restores the deep-sleep
bits under fresh quiescence checks. A partial restore retains the policy.
The table-only pre-reset reconciliation path refuses retained deep-sleep
ownership; it cannot falsely retire a clock policy by completing a DMA table.

## DF C-state remains a separate requirement

[`smu_v13_0_0_set_df_cstate`](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0_0_ppt.c#L2846)
uses `SetExternalClientDfCstateAllow` (`0x3b`). The
[`pp_df_cstate` enum](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/include/kgd_pp_interface.h#L249)
defines disallow as 0 and allow as 1. Linux
[suspend and unwind](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu/amdgpu_device.c#L3045)
write these explicit policies, rather than reading an earlier allowance.

The inspected interface and call paths provide no getter for that previous
external-client allowance. Enabled feature bit 41 is not an observation of
the allowance. An ACK to `0x3b,0` would establish a requested inhibit; a later
ACK to `0x3b,1` would establish allow, but would not prove restoration of the
previous policy. The original deep-sleep snapshot issues neither command.
The subsequent [owned DF policy](smu-df-cstate-policy.md) now defines explicit
DISALLOW acquisition and terminal ALLOW release; it does not claim prior-policy
restoration. The complete `ColdFixedDcCeilingV1` transaction still needs its
actual caller and retained observations.

## Validation

`build/smu-deep-sleep-local/report.json` records strict C++11 and UBSan runs,
each with 138,600 passing checks. These are offline protocol tests, not GPU
execution. They cover all four original bit combinations, preserved unrelated
features, zero-mutation cases, exact restoration, range ordering, reentry,
feature changes, every transport read/write/validity boundary, late failures
after landed writes, bad firmware responses, deadlines, and clock reversal.

After the independent source review, the focused native check passed on the
Intel Mac (macOS 15.8 build 24H22, SDK 26.2). Four affected objects compiled
against actual kernel headers with the strict 2 KiB frame limit; the changed
suite passed the same 138,600 checks under ASan/UBSan. No kernel was loaded
and no GPU was used. The 88-file source archive was hash-verified; its temporary
2,548 KiB stage was removed.

Evidence: `build/native-smu-deep-sleep-final/report.json` and
`post-validation.json`. Existing diagnostic bundles and exported native
artifacts remain immutable; this focused check did not relink the full kext.
