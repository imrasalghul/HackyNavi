# Native clock ownership for the initial DCN3.2 mode

This began as a source audit and implementation recommendation. The narrow
UCLK implementation described below is now present; the complete clock
transaction is not implemented and no target clock operation is claimed.
Production sources were unchanged during the audit phase. All Linux references below use
commit `3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`; local line numbers refer to
the unmodified files under `.research`. Thirteen files were freshly fetched
and matched after CRLF normalization; see
`build/display-clock-hold-source-audit.json`.

## Concrete conclusion

### Implemented FAMS coordination (2026-10-11)

`HackyNaviDisplayController::disableFamsForColdClockChanges` now binds the
actual normal FunctionGroup Operation and GART scope, verifies its owned
DMCUB inbox is idle, and requires all physical pipes and local scanout/link
owners idle. One elapsed deadline covers all mailbox, ring-memory and flush
callbacks. It sends DAL `0x14,0` and receives its ACK **before** submitting
DMCUB command type76/subtype0 with the exact52-byte disabled configuration.
The command occupies one64-byte ring slot and must be consumed by firmware.
This follows the pinned `dcn32_prepare_bandwidth` / `dcn32_p_state_delegate`
ordering. No firmware response field is invented for that command.

The DAL policy journal records the request before a potentially delivered
mailbox operation. Any attempted policy change makes minimum-release require
verified reset; it cannot claim to restore an unknown previous policy. The
display journal records DAL acknowledgment, slot writing, submission and
consumption independently, retaining the hardware owner after partial or
ambiguous completion. `Inbox::checkIdle` performs fresh read-only ownership
checks without clearing the live ring owner.

Independent review found a deadline gap in existing DMCUB memory callbacks;
the corrected callbacks now check scope/deadline before and after copies,
synchronization and HDP flushing. Strict compilation also exposed an existing
2,584-byte EDID stack frame, replaced by a bounded owned scratch allocation
with cleanup on every exit.

Evidence: `build/fams-local/pinned-layout-report.json` checks the exact pinned
wire definitions; `build/fams-local/independent-review.json` reviews the final
sources. `build/native-fams-final/report.json` records19,417 DMCUB and9,953 DAL
ASan/UBSan checks with zero failures, plus three actual Kernel.framework
object compiles under the2KiB frame limit. The63-input archive was verified
and the1,992KiB Mac stage removed. No GPU firmware command has been executed
by these tests. There is no automatic startup/modeset caller yet. Full clock
ownership and the DML consumer are still required.

The subsequent `requestMemoryMinimum` integration now accepts a Ready DMCUB
only when this same owner retains the completed FAMS and DAL journals in the
current Operation generation. It verifies the actual firmware and owned idle
inbox before the table query, before the minimum request and after completion.
All physical pipes/local consumers still must be idle. The Prepared path
retains its actual reset/disabled readbacks. Real table/DC bounds, monotonic
floor policy, elapsed deadline, fulfillment and reset-only retention are
unchanged; no fixed clock or DML authorization is inferred.

`build/fams-uclk-kernel-check/vcn-independent-review.json` covers that bridge.
Its actual Kernel.framework object compiled under the 2 KiB frame limit in
`build/native-fams-uclk-final/report.json`; the 604 KiB stage was removed and
all archived inputs verified. Unchanged portable protocol suites were not
repeated. This bridge is later than the `macos-accelerator-final` full kext and
has not run on hardware.

The existing DAL owner can be extended to UCLK with an actual fulfillment
check. It cannot establish the profile's independent SOCCLK/FCLK requirements
by merely adding enum values: DAL defines no fulfillment bits for those two
clocks. The shortest sourced next owner therefore combines DAL with the
existing exclusive PPSMC owner and a real SMU metrics transfer allocation.
Neither an ACK nor a DPM inventory is a measured clock or an enduring hold.

`ColdFixedDcCeilingV1` remains an explicit calculation policy. Its selected
SOC/Fabric/Memory values are requirements, not an observed simultaneous state.
The trusted consumer must retain the complete profile, actual calculated
clock record, and the concrete owner which maintains them. Legacy DP/TMDS
plans currently set `dtbclk_required=false`: their bounding-box DTB value is
not permission to activate a DTB consumer.

## Mailboxes and command contracts

| Purpose | Actual source-backed command | Completion and limits |
| --- | --- | --- |
| DAL DISP/DPP/DCF/DTB minimum | `0x09`, `(PPCLK << 16) | MHz`; IDs 8/9/11/12 | ACK 1, then `0x15` fulfillment. Require DPREF `0x04` plus respective bits `0x01/02/08/10`. Navi31 PMFW must be at least `0x4e6a00`. |
| DAL memory minimum | Same `0x09`, PPCLK 2 | ACK 1, then fulfillment mask `0x24`. Input is UCLK MHz, not DRAM MT/s. Current GDDR6 model converts UCLK to DRAM rate by 16. |
| DAL memory maximum | `0x0a`, `(2 << 16) | MHz` | ACK only; no hard-maximum fulfillment query is defined here. Linux uses this to restore a top limit or set its DC soft-max policy. |
| DAL FCLK switching policy | `0x11`, 0 disallow / 1 allow | Policy ACK only, no numeric FCLK value and no fulfillment bit. |
| DAL firmware MCLK coordination | `0x14`, 0/1 | Controls whether PMFW always waits for DMCUB. It does not itself disable DMCUB FAMS. |
| DAL DCF deep-sleep floor | `0x0d`, MHz directly | Separate ACK-only requirement; normal DCF minimum does not establish it. |
| DAL display count | `0x0e`, actual count | Linux sends active count before raising/activating and zero on the safe-to-lower display-off path. |
| PPSMC SOC/Fabric soft range | `0x1a` maximum, then `0x19` minimum; `(PPCLK << 16) | MHz`; IDs 1/3 | Linux's actual forced-level path. Maximum is encoded as requested MHz **plus one**; minimum is unrounded. ACK only, not DAL fulfillment. |

DAL uses MP1 C2PMSG 74/51/52; PPSMC uses 66/82/90. Serialize both through their
actual owners, preserving their independent command maps. In particular,
DAL `0x0c` means DC ceiling while PPSMC `0x0c` reads enabled features.

Sources: `.research/linux/dalsmc.h:37`,
`dcn32_clk_mgr_smu_msg.c:193` and `:281`,
`dcn30_clk_mgr_smu_msg.c:230` and `:296`,
`smu_v13_0.c:1406` and `:2231`,
`smu_v13_0_0_ppt.c:1942`, and
`.research/clock-hold/smu_v13_0.h:65`.

Do not reuse the display `reportedKHz` check for UCLK without establishing
its reply contract. The pinned helper's comment says MHz, its log says kHz,
DTB's caller consumes the result as kHz, and UCLK's callers ignore it.
Preserve the UCLK reply as raw data and use the explicit UCLK fulfillment
bit; establish its current frequency through the metrics path below.
See `dcn32_clk_mgr_smu_msg.c:280–301` and `dcn32_clk_mgr.c:725–776,817–824`.

## Required PPSMC and observation owner

Read the real interface/version and running feature mask. The pinned
SMU13.0.0 table layout has interface `0x3d`; SOC/FCLK/UCLK DPM bits are 5/4/3.
Reject unsupported interface/layout and disabled DPM for this first profile;
do not port Linux's disabled-DPM setter return of zero as successful clock
ownership. Linux's version-check function only logs the interface, so an
exact-layout gate is an additional port requirement, not an upstream check.

For actual observations, allocate and retain the firmware-visible metrics
table in owned VRAM, aligned to a page, with the exact
`SmuMetricsExternal_t` size. PPSMC `0x0e/0x0f` publish its GPU address;
`0x12`, table ID 5, requests firmware-to-memory transfer. After ACK, perform
the real HDP invalidation/CPU synchronization before reading `CurrClock[1]`,
`[2]`, `[3]` (and DCF `[11]`). Bypass the Linux helper's one-millisecond cache
for transaction observations. Keep the allocation alive after publication,
including failures whose DMA completion is unknown. A timeout is not a
retirement proof.

Sources: `.research/clock-hold/smu13_driver_if_v13_0_0.h:27,51,440,1424,1487,1611`;
`.research/linux/smu_v13_0_0_ppt.c:480–485,727–768,995–1042`;
`smu_v13_0.c:594–612`; `smu_cmn.c:744–809,1109–1157,1197–1224`.
The existing `smu::State` owns version/GFXOFF state but currently has no
soft-range or metrics-transfer API.

SOCCLK is not requested by Linux's DCN32 DAL path
(`dcn32_clk_mgr.c:707–709`), and FCLK there is controlled as switching policy.
Using the separate PPSMC forced-range mechanism for this port's fixed profile
is an explicit implementation choice, not a claim that Linux DAL already
does it. The low/high feature masks also expose SOC/Fabric deep-sleep bits
12/13; neither an ordinary soft minimum nor one `CurrClock` sample proves
that all deep-sleep behavior is excluded. The owner must retain a sourced
compatible deep-sleep/DF-Cstate policy or fail this fixed-clock authorization.
Do not disable arbitrary feature masks as a substitute.

## Acquisition and recalculation sequence

1. Under the actual normal FunctionGroup operation and installed GART access
   guard, retain Display/Firmware/GART, SMU owners, mode, and surface ownership.
   Validate the retained input serial and generation. Require hardware-verified
   all-pipe/DTO/stream-off state, plus owned DMCUB/FAMS state; software counters
   alone do not establish this. Keep fetch disabled throughout acquisition.
2. Build a private command journal before the first write. Mark each possible
   policy/range/table publication before issuing it. Use one real elapsed-time
   deadline, validity checks around accesses, bounded response polling, and
   fail on every non-OK response or lost scope. No userspace-supplied clock ID,
   address, arbitrary table bytes, or fabricated observation may drive this.
3. Establish the fixed policy and numeric requirements through real owners.
   Extend DAL for UCLK/fulfillment first. For UCLK below the physical top, a
   minimum alone does not establish fixed frequency: the maximum policy and
   its observed result must also be owned. Add the PPSMC SOC/FCLK range and
   fresh-metrics owner before treating those clocks as fulfilled. DCF's
   calculated deep-sleep floor is a separate command. FCLK switch-disallow
   is not a substitute for the numeric Fabric requirement.
4. For DISP/DPP, use the VCO-derived dentist preset and floor its MHz encoding,
   as the source requires; DCF/UCLK/DTB use their source's ceiling/integer rules.
   Preserve the source's preliminary divider-126 step when entering divider
   127. All pipes off removes its per-active-pipe FIFO work, not the command
   contract. No active DTO lowering is permitted by this first owner.
5. Check DAL fulfillment and fresh metrics while the constraints remain owned.
   Then recapture board/DPM/DC ceilings/PLL/timer through the existing native
   capture, and recalculate against that new record. This is required because
   clock mutations invalidate the old DPM observation. If the actual selected
   or reference clock differs from the calculation, recalculate or reject;
   do not patch only its provenance IDs.
6. Retain the hardware clock owner across an isolated userspace calculation;
   do not hold a thread-bound FunctionGroup scope across IPC. On return, take
   a new real scope and revalidate its generation, owner, accepted command
   journal, fresh completion/observations, and captured calculation serial.
   Only that concrete owner can authorize bandwidth/DTO/OTG programming.
   A stale reply, expired request, PM transition or reset invalidates it.

Clock raise/lower ordering and dentist rules are in
`dcn32_clk_mgr.c:309–323,364–484,639–857`.
Linux prepares clocks before watermarks (`dcn20_hwseq.c:2378–2426`) and
lowers only after the corresponding safe update (`:2429–2476`). A cold
transaction is a restricted path; it does not implement those active-mode
transitions.

DMCUB coordination cannot be replaced with an invented generic clock-notify
command. DCN32's actual FAMS transition forces memory switching unsupported
before the bandwidth update, then disables delegation for the non-FAMS state
(`dcn32_hwseq.c:1815–1844`). The DMCUB command is type 76,
`FAMS_SETUP_FW_CTRL`, with `fams_enabled=false`, followed by a real wait for
consumption (`.research/clock-hold/dc_dmub_srv.c:447–514`). That wrapper returns
true without propagating its send result: a new owner must check the actual
transport result. The separate DAL `0x14` flag must agree with the retained
FAMS state. If DMCUB initialization cannot establish that state, acquisition
must stop before memory-policy writes.

## Rollback, terminal release, and unresolved claims

Failure after any possibly delivered write leaves the transaction faulted and
its resources retained. Do not mark a clock released because a later read or
reverse write failed. Under actual RetirementScope, first prove every consumer
and firmware activity quiescent; then restore only exact prior settings owned
by this driver, or execute a separately specified terminal policy. Numeric
DPM limits and current clock samples cannot reconstruct another client's
previous soft/hard constraints.

The PPSMC automatic-range path is explicit: soft maximum `0xffff` followed
by soft minimum zero, after validating actual enabled DPM and exclusive
ownership (`smu_v13_0.c:1424–1444,1491–1504`). It restores automatic policy,
not a captured foreign range. The sourced DCN32 terminal memory operation
lowers the minimum to actual DPM0 and restores maximum to actual highest DPM
(`dcn32_clk_mgr.c:1010–1039`). Neither action proves stopped clocks or retired
DMA. FCLK/FAMS changes must follow the owner’s known previous policy; do not
blindly enable transitions during failure cleanup.

**Provenance gap in the existing release claim:** this audit did not find an
explicit pinned DCN32 call or specification establishing DAL hard-min zero as
full withdrawal. `src/display_smu.cpp::releaseMinimaAfterQuiescence` currently
sends zero. Do not extend that claim to the new memory/SOC/Fabric transaction
without further firmware evidence. Keep its state/resources until a sourced
terminal policy or actual verified reset handles them.

The implementation order is DAL UCLK completion and private command journal;
PPSMC feature/range/metrics ownership; known FAMS and
deep-sleep policy; then native recapture/calculation binding and modeset.
The cold UCLK stage below and the separate native PPSMC clock/metrics owner
are now implemented. See [PPSMC validation](validation-smu-clock-owner.md);
the coordinated DML/modeset transaction remains incomplete.

## Implemented cold UCLK stage

`displaysmu::Controller::requestMemoryMinimum` sends the actual PPCLK2
minimum and polls both fulfillment bits. Its private journal records a possible
request before the message write, then distinguishes ACK, successful raw reply
read, and fulfillment. An earlier fulfilled floor survives a failed later
request. Raw zero is accepted as uninterpreted data; all-ones/failed reads,
rejected responses, absent completion bits and exhausted budget fail.
Requests are nonzero and monotonic. UCLK does not enter the existing display
frequency API, and no MHz/kHz interpretation is applied to its raw reply.

`HackyNaviDisplayController::requestMemoryMinimum(Operation&, MHz, timeoutUs,
MemoryMinimumResult&)` is callable only in Prepared, before this owner boots
DMCUB. Actual reset/control/reset observations establish DMCUB disabled and
held in reset. Hardware admission verifies all physical OTG/HUBP/MPCC pipes,
with safe handling of powered-off HUBPs. Local DTO/link/stream owners must also
be absent; this does not claim inherited DTO/PHY register verification. The
same normal FunctionGroup scope, installed GART guard and elapsed deadline
cover the owner-lock wait, all MMIO and repeated admission after completion.
The private actual DAL inventory bounds the requested floor; inventory alone
never establishes a hold. Retention precedes any DAL write.

A UCLK constraint survives DMCUB boot without a duplicate hardware retain.
The additive `boot(Operation&, Poll)` overload is required in this case and
checks the actual provider/generation and fulfilled journal. Existing no-UCLK
boot callers retain their old API; Engine now passes its existing actual
startup scope through the new overload without making a UCLK request.
Normal stop returns a retained failure:
the portable release method returns `ResetRequired` before any release write
when UCLK may be active. Only the existing full-device verified-reset path
clears that journal. The older display-only zero-command behavior is unchanged
and explicitly labelled unproven as full withdrawal.

There is no Engine/Device callsite for the UCLK request, no post-boot FAMS
transition or memory hard maximum. A separate PPSMC SOC/FCLK and metrics owner
is now native-linked; that does not complete the coordinated transaction. It is
not yet a fixed-clock DML authorization or tested GPU operation. Focused
sanitizer and real KernelSDK compile evidence is recorded in
`build/display-uclk-host-review.json` and `build/display-uclk-native-review.json`.

Primary source links:
[DCN32 clocks](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/clk_mgr/dcn32/dcn32_clk_mgr.c),
[DAL completion](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/clk_mgr/dcn32/dcn32_clk_mgr_smu_msg.c),
[SMU13 ranges](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0.c),
[Navi31 SMU tables](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0_0_ppt.c),
[metrics transport](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/pm/swsmu/smu_cmn.c),
[firmware ABI](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu13_driver_if_v13_0_0.h),
[DMCUB](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/dc_dmub_srv.c).
