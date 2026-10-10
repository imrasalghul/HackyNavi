# Coordinated Display and Firmware clock ownership

The native Display owner now exposes SOC/Fabric soft-range, SOC/Fabric deep-sleep
inhibit, and external-client DF C-state disallow operations through the existing
Firmware/PPSMC protocol implementation. The old Firmware entry points still
require no Display owner and actual disabled/reset DMCUB readbacks. No engine
startup callsite is changed.

The coordinated route accepts the actual retained Display object, not a copied
FAMS result or an authority boolean. It requires that object's Ready DMCUB,
hardware retain, GART lease, completed same-generation type76/subtype0 consumption
and DAL0x14 acknowledgement. The private FAMS and DAL journals are read under
Display -> Firmware -> GART locks. The caller's real FunctionGroup Operation and
normal GART::Access must already be installed. No descendant method acquires
another FunctionGroup operation or recursively locks Display.

Queue and VM owners remain excluded. So do a native scanout owner, owned AUX/DDC
pins/controllers, links, streams, and DTOs. Every boundary reads the physical
pipe-idle protocol and brackets it with this inbox's actual DMCUB enable/reset/
boot state, mapping, producer and RPTR checks. The running DMCUB route does not
replace those checks with reset. The existing SMU awake checks still run.
The cached DPM/reference observation is invalidated before any possible policy
publication and during coordinated retirement.

One measured start and timeout is shared by Firmware's clock callbacks and
Display's existing guarded register callbacks. The timestamp is captured before
the Display lock; bounds include lock acquisition and all preflight, command,
and final checks. Both guards retain exact lexical scope pointers and preserve
their first error. Their bindings are cleared before descendant locks are
released. Timeout, scope loss or post-command ambiguity cannot return success;
faulted owners and their hardware journals remain retained.

## Matching retirement

The new `retireFirmwareClockState` entry point requires an actual callback-issued
RetirementScope and retirement GART access. Its generation is newer than the
retained FAMS/Firmware clock generation, which must still agree with each other.
This follows FunctionGroup's real revocation/worker protocol; old journals are
not relabelled with the new generation.

While the same Display remains Ready and all the above consumers are absent,
the bridge uses the existing protocol methods in this order:

1. Seal a metrics producer only after its real completed transfer/readback.
2. Return touched SOC/Fabric ranges to the sourced automatic sentinel policy.
3. Send the owned external-client DF ALLOW policy.
4. Restore only the SOC/Fabric deep-sleep bits previously captured by this owner.

Fresh quiescence checks surround each actual transition. The bridge never clears
the FAMS, DAL, UCLK, buffer or DMA journals, never releases Display's lease, and
never performs or asserts a device reset. Firmware becomes Quiesced only after
its own policies are terminal. This allows the separate existing stop/reset
owner to continue without requiring Display to disappear before the Firmware
policy can be withdrawn.

The future coordinator must call this retirement entry **before** stopping the
owned DMCUB or trying to release Display. It must first retire actual graphics,
VM and display consumers. A queue fence alone is not sufficient. The current
engine creates graphics/VM owners before Display boot; this bridge does not
make that order eligible for cold clock acquisition. Startup integration must
establish the required order separately.

Ambiguous or faulted policy/firmware state is not repaired by this bridge. Such
a state remains faulted/retained and may reach the existing fail-stop policy;
no successful-zero response, invented reset certificate, or unknown foreign
policy restoration is supplied.

## Evidence boundary

The bridge reuses the existing pinned protocols: `smu_clock.cpp` references Linux
3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0 for PPSMC soft ranges, feature masks and
DF policy; `dmcub.cpp` uses the actual DCN3.2 mailbox command and mapping checks.
This change adds native ownership and sequencing, not new register values or
portable transport implementations. Native header compilation and source review
do not establish target execution, fixed/measured clocks, bandwidth-model
validation, scanout, or Metal graphics rendering.
