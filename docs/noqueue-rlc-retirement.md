# Firmware-owned retirement before a queue exists

This is a separate checked early-startup teardown route. Engine admits it only
when no GraphicsQueue or Display object exists, the actual Firmware owner is
healthy `FirmwareLoaded`, and the actual GART is `BothEnabled`. It does not
change reset permissions or add a startup operation.

Under Firmware -> GART locks, the owner requires completed real RLC autoload,
SMU awake, its hardware/lease holds, live PSP ring and TMR bindings, no pending
PSP command, no queue/Display lease, no native VM owner, and no active or failed
VM retirement. All owned PSP/TMR allocation handles must still resolve to their
actual private ranges. The caller's genuine RetirementScope and installed GART
access govern every register access and the single callback deadline.

Before its first MMIO, Firmware withdraws readiness so no later queue can bind.
The portable `gfxinit::AutoloadRetirement` journal is independent of the existing
prepared-CSIB State: it cannot manufacture a CSIB allocation or prepared state.
It latches attempted before the first register read. Every attempted failure is
terminal and keeps the actual Firmware/TMR owner retained.

The physical protocol requires halted CP/MEC/MES, inactive graphics rings,
VMID0 enabled and all other VMIDs disabled, the default selector, exact zero
CSIB high/low/length, and both SRM enable bits clear. These are fresh checked
entry conditions, not assumptions about what autoload leaves behind. Unknown
or nonzero CSIB is rejected without writes and is never cleared or adopted.
RLC safe-mode acknowledgement and RLC enable clearing use the same helper as
existing owned-CSIB retirement. The full GPM/SRM/FIFO/RLC/MC/translation/GRBM
physical drain and zero-binding/disabled-SRM observations are repeated before
success. The new path never writes CSIB or SRM.

The original owned-CSIB route retains its exact allocation match, SRM disable,
CSIB length-first clearing, final drain and all original ordering. Both routes
now set a separate native `rlcRetired_` latch only after actual success. PSP
DESTROY_TMR is forbidden for autoloaded firmware without that latch. Generic
`Quiesced` (also reached by clock cleanup) is insufficient. The later PSP
command completion, GART retirement, NBIO aperture disable and BME withdrawal
remain independent operations in their established order.

Engine's stop attempt remains one-shot. No queue means no queue completion is
invented: Engine skips queue/VM calls and invokes this Firmware method at the
RLC retirement stage after inherited scanout shutdown. The existing healthy
Prepared/Running queue route remains unchanged except for setting the same
physical-completion latch. Parent cleanup still preserves errors and holds.

## Source and limits

Pinned Linux `3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`,
`gfx_v11_0.c:2306-2377` defines CSIB, RLC stop and SRM control;
`:2522-2531` shows CSIB/SRM installation after PSP firmware loading;
`:5445-5463` supplies safe-mode command and acknowledgement. The generated
GC11 masks supply the existing conservative physical drain checks. This port
policy is not a claim that Linux's ordinary hw_fini proves all these drains.

No target evidence establishes that the new strict zero-CSIB/SRM-disabled and
halt guards pass after actual autoload. Unknown inherited CSIB, active/unhalted
CP, failed GFXHUB setup, failed/ambiguous autoload or SMU initialization, a
faulted/allocated-only queue, and earlier PSP startup failures remain outside
this route. Termination/close/shutdown still forbid reset recovery and can
therefore reach the existing fail-stop if ordinary retirement fails. Reset-only
DAL/FAMS policy remains a separate unresolved lifetime constraint.

Tests exercise the actual portable protocol with the existing host-only MMIO
model: both RLC enabled/disabled observations, every transport failure and
all-ones read, landed false writes, fresh and changed CSIB/SRM, physical guard
and individual drain bits, exact polling boundaries, retry rejection and invalid
preflight. Tests do not create a prepared CSIB state for this route. Real SDK
kernel-object compilation checks integration and stack limits; neither activity
is GPU execution or proof of target boot safety.
