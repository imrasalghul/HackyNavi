# Integrating the coordinated cold clock owner

The native clock bridge, its matching retirement and the Display consumer /
DMCUB stop split are implemented. Engine now calls the two Display phases on
opposite sides of actual graphics/VM/RLC retirement. Startup order is unchanged
and no cold clock policy acquisition has been activated. No queue-fence
observation is promoted to global quiescence.

## Current dependencies

`HackyNaviEngineController::start` loads PSP/SMU/GC/DMUB firmware, enables
GFXHUB, creates/prepares/starts GraphicsQueue, and runs its write, shader and
reuse probes before creating Display and booting DMCUB. The new bridge rejects
that queue owner even when a fence completed. It also rejects every registered
VM owner, including a reusable context whose current job has been consumed.

The former monolithic Display stop ran before GraphicsQueue and would stop
DMCUB before coordinated clock retirement. The split now preserves its live
same-owner inbox through that interval. `GraphicsQueue::retireAfterQuiescence`
sets Firmware to Quiesced and
clears `gart_->firmwareReady_` before real RLC retirement. Native scanout
retirement requires that readiness flag; therefore scanout must be retired
before RLC retirement. The clock bridge deliberately accepts Firmware Quiesced
under a genuine RetirementScope while its own DMCUB is still Ready.

## Preparation order for a future cold coordinator

1. Preserve the current first-attach authority, inherited scanout/console and
   DMA retirement, reservations, GART/MMHUB and real firmware-load prerequisites.
2. After `loadGraphicsFirmware` succeeds, create the actual Display owner before
   creating GraphicsQueue or any VM context. The Display factory accepts the
   existing loaded DMUB firmware and its reserved Display GART allocation; it
   does not require a GraphicsQueue. GFXHUB can be enabled as required, but does
   not itself authorize a queue or VM owner.
3. Obtain any pre-boot DAL requirements through the existing Prepared/reset
   paths and explicit same-generation Operation. Boot DMCUB with that Operation.
   No frequency constant may be invented merely to satisfy this step; the
   eventual request must come from the owned model/validated mode requirements.
4. With every physical pipe cold and every local consumer absent, perform the
   actual DAL0x14 + FAMS-disable consumption handshake. Obtain and validate the
   real DPM/DC inventory, retained ROM references, programmed clock observations
   and model inputs needed for the selected mode.
5. Acquire the sourced SOC/Fabric DS policy before ranges, the external-client
   DF disallow policy before ranges, and then the required SOC/Fabric ranges
   through Display's bridge. Use the existing UCLK/DCF DAL APIs for their own
   contracts. Their acknowledgements, fulfilled UCLK floor and programmed DFS
   observations remain distinct evidence; none alone establishes a fixed-clock
   operating point or DML acceptance.
6. Only after that transaction's complete admission is implemented may Engine
   create graphics/VM owners and run the existing actual GPU probes. Scanout,
   link activation and accelerator publication must remain after their separate
   required proofs.

The serial/epoch identity of every retained observation must be rechecked inside
the transaction. The new bridge invalidates old DPM/reference observations when
it changes policies; a coordinator cannot reuse a formerly valid snapshot after
those writes without the required fresh capture. This order is not an enduring
bandwidth certificate and does not bypass the unfinished mode-model integration.

## Retirement order and required Display split

Use the actual callback-issued RetirementScope and installed retirement GART
access. Normal admission has already been revoked and its Operation drained.
Descendant calls remain serialized by their existing owner locks; no lock is
held across another method that reacquires it.

1. Stop inherited boot scanout using its existing captured owner. Stop native
   streams and retire every native scanout surface using its real blank/drain,
   HUBP/pixel and allocation-release protocols. Clear `scanoutOwner_` only via
   that owner's successful release. This must occur while normal firmware
   readiness still exists; do not move it after RLC retirement.
2. Split the consumer portion of Display's existing `stop` from the portion
   that stops DMCUB. The consumer portion closes owned AUX/DDC, disables actual
   DP/HDMI links, stops DTOs and completes the physical idle/power transitions.
   It must leave DMCUB, its inbox mapping, FAMS/DAL journals and Display retain
   intact. The split must reject a remaining surface, stream, pin/controller or
   link owner and preserve retention on every partial failure. Merely adding a
   boolean `consumersStopped` is insufficient; the bridge still performs its
   own physical and ownership checks.
3. Stop GraphicsQueue through the existing hardware protocol. Invoke
   `retireCompletedContexts` to retire actual VM registry owners with queue
   provenance and both hub detaches, then `retireAfterQuiescence` for real RLC/
   SRM/clear-state retirement and queue-lease release. A stopped ring or latest
   fence alone does not clear these owners. Verify the actual queue/VM registries
   are empty, as the bridge independently requires.
4. While Display's DMCUB remains Ready, invoke
   `display_->retireFirmwareClockState(scope, budget, result)`. It seals the
   completed metrics producer, returns touched ranges to automatic, sends DF
   ALLOW and restores only captured DS bits. It rechecks the same private FAMS
   generation, real inbox, physical pipes and deadline around each transition.
   Firmware Quiesced is permitted for this cleanup; normal work is not reopened.
5. Only now execute Display's DMCUB STOP_FW/PWAIT/reset/window-disable portion.
   Its policy-retention result must remain explicit. FAMS and reset-only DAL
   requests currently prevent `releaseLeaseLocked` from returning success even
   after DMCUB stops. Do not force-clear them or reinterpret Busy as retirement.
6. If separately authorized, use the existing actual full-device reset proof to
   release those remaining owners, then complete ordinary Firmware/GART/NBIO
   retirement. The clock bridge has removed its own policy dependency before
   the reset preparation path; it does not supply the reset proof.

## Implemented Display phase contract

`stopConsumers(RetirementScope&, timeoutUs, poll, StopResult&)` uses the existing
actual AUX/DDC, DP/HDMI, DPP DTO and domain-power retirement operations. Native
stream and surface ownership is rejected before link teardown. Successful
quiescence leaves Display Ready and does not change its DMCUB runtime, inbox,
FAMS/DAL journals or hardware retain. A Prepared owner without a hardware hold
performs no MMIO. There is no retained `consumersStopped` authority bit.

`stopFirmware` has the same scoped signature. It independently rejects actual
queue/VM and local consumer ownership. With a started private FAMS journal it
calls the existing private Firmware clock retirement while retaining Display's
lock, dropping Firmware/GART locks before the nested owner acquires them. The
same elapsed deadline, including lock wait time, covers admission, clock
retirement and actual DMCUB STOP/reset. After the nested call it rechecks the
scope, deadline and physical/local prerequisites. Without FAMS it stops/releases
Display first; Engine then invokes the existing direct Firmware clock retirement.

Global physical idle remains mandatory where actual owned DTO/domain or
FAMS/DAL/minimum journals require it. The DMCUB-only no-policy path retains its
old treatment of inherited downstream OTG/MPC/DIO, which are not DMCUB runtime
DMA clients. Such a stop does not publish `physicalIdleChecked=true`.

Actual STOP_FW/PWAIT/reset/window readbacks remove only runtime retention.
`firmwareStopped` reflects the actual portable Stopped phase; a never-booted
Prepared owner does not fabricate it. Reset-only DAL/FAMS ownership returns
Busy with `policyRetained=true`, the lease retained, and append-only
`State::StoppedRetained`. It is not marked Faulted merely for requiring reset.
FaultedRetained is still excluded from both new APIs; a lost callback scope or
ambiguous hardware operation retains ownership. Only the already-existing
independently verified full-device reset path clears policy journals.

The legacy `stop(poll)` composes the same private cleanup with its original
no-policy semantics. It rejects started FAMS before I/O, because it lacks the
genuine RetirementScope needed to preserve and retire coordinated clock policy.
All StopResult fields are observations, not a capability to bypass later checks.

Local validation compiles the affected native code with actual kernel headers
and the 2 KiB stack-frame limit. Extended portable DMCUB tests revoke admission
at every STOP read/write boundary, check retention through failure and a second
still-revoked attempt, then require the actual stop protocol to complete under
new admission. They also verify no extra I/O on repeat STOP and preservation of
PSP secure-window/code ownership. These are offline tests, not target hardware
or native IOKit-object execution.

## Remaining lifecycle blockers before activation

Engine now admits one narrow partial-start cleanup only with actual Running
GraphicsQueue, FirmwareLoaded Firmware, BothEnabled GART, and absent/Prepared/
Ready Display. Earlier failures without the real queue/RLC retirement owner,
ambiguous child states and repeated cleanup remain reset-only. This does not
yet supply a cleanup route for early coordinated clock acquisition before a
GraphicsQueue exists. The future startup transaction must solve that case;
an Engine stage number or failed activation is not stopped-hardware proof.

If a clock command, queue retirement or DMCUB state is ambiguous, the bridge
does not rehabilitate its faulted journals. Existing pre-reset preparation also
refuses still-active firmware range/DS/DF policies. A future recovery extension
requires separate source-backed authority; bypassing that refusal is not part
of this design.

FAMS/reset-only DAL policy retirement currently depends on full reset.
FunctionGroup intentionally does not permit PCI recovery for termination/close
callbacks that may hold the PCI workloop. Consequently this ordering is viable
only where its final required reset is genuinely permitted. Before enabling the
new policy path as a general device lifecycle, implement a source-supported
terminal policy release for the other callbacks, or another proven lifecycle
route. Do not claim the helper closes that broader lifecycle gap.

These conclusions are based on the actual Engine, GraphicsQueue, ScanoutSurface,
Display and FunctionGroup source; no GPU or target execution was performed.
The original design evidence remains preserved at
`build/display-firmware-clock-bridge-local/coordinator-design-evidence.json`;
the stop implementation has separate evidence under `build/display-stop-split-local-2`.
