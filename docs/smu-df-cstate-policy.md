# Owned DF C-state allowance

The existing Firmware/PPSMC clock owner now implements one-shot external-client
DF C-state inhibition and terminal release. This is a clock-policy prerequisite;
it is not a completed display transaction or evidence of GPU execution.

## Sourced commands and deliberate release policy

Pinned Linux commit `3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0` maps
[`smu_v13_0_0_set_df_cstate`](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0_0_ppt.c#L2846)
to `PPSMC_MSG_SetExternalClientDfCstateAllow` (`0x3b`), with DISALLOW=0 and
ALLOW=1. Its suspend/unwind path uses those explicit policies. There is no
inspected getter for the previous external-client allowance. Feature bit 41
describes the enabled feature and cannot supply that missing observation.

`disallowDfCstate` sends `0x3b,0` through the existing serialized SMU mailbox.
`allowDfCstateAfterQuiescence` deliberately ends this driver's ownership by
sending `0x3b,1`. The latter is a new ALLOW request, not restoration of an
unknown earlier policy. Neither operation writes an enabled-feature mask.
The journal distinguishes attempted command, firmware ACK, surrounding feature
checks, and successful completion; it does not call an ACK a measured DF state.

## Ownership and ordering

Both operations retain the existing firmware version/interface and GFXOFF
requirements. They read the complete feature mask twice before and twice after
the policy command and require agreement. An owned SOC/Fabric deep-sleep hold
must still match its observed feature mask. This establishes sampled consistency
under the serialized owner, not continuous hardware observation.

The native entry point holds a real FunctionGroup operation, Firmware/GART
locks and one total deadline. Fresh checks on both sides require no queue,
VM or display owner, idle physical pipes, and reset/disabled DMCUB. It is not
automatically invoked by boot or the diagnostic driver.

The controller journals before entering mutation transport, including failures
which might occur after a write lands. Any transport error, bad response,
deadline, lost scope, changed feature mask or failed native postcondition
faults the common firmware authority. An ambiguous or incomplete DF command
retains its policy journal without retry. If terminal ALLOW already completed
before the native postcondition failed, that acknowledged history remains
recorded; the common native authority and resources still remain faulted and
retained. A fresh controller cannot claim the previous operation.

Native retirement first closes the metrics producer and returns owned clock
ranges to automatic policy. It then requests DF ALLOW under fresh quiescence
checks, and finally restores only the captured SOC/Fabric deep-sleep bits.
Terminal DF release prevents a later range request or new deep-sleep hold.
The table-only pre-reset path refuses retained DF policy; completing a table
transfer cannot retire this independent policy owner.

## Current validation

`build/smu-df-cstate-local/report.json` records strict C++11 and UBSan runs,
each passing 153,589 checks. These are offline protocol tests, not firmware
execution. They cover both states of feature bit 41, acquisition before/after
the deep-sleep hold, exact command arguments, metrics/range release ordering,
reentry, changing masks, every transport read/write/validity boundary, late
failures after writes, deadlines and reversed clocks.

Independent review of freeze3 passed after two corrections: the final native
deadline/scope check now faults the common owner, and the documentation preserves
an already completed ALLOW when a later native postcondition fails.
`build/native-smu-df-cstate-final` then passed the same 153,589 checks under native
ASan/UBSan and compiled four kernel objects with a 2 KiB frame limit. Root
verified all 88 archived source members and the object hashes recorded in the
native log; the 2,616 KiB stage was removed. No kernel was loaded and no GPU was
used. The policy is now integrated in the complete
`build/macos-metal-clock-policies-final` kext (94 units/412 resolved imports).
Previous deep-sleep/native archives remain unchanged; neither build has run on
the target GPU.
