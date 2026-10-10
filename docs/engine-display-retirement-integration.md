# Engine retirement with a running display firmware owner

Engine now retires Display consumers before graphics and VM owners, and keeps
DMCUB alive until the coordinated firmware-clock release can complete. Its
startup order and clock-policy activation are unchanged. This integration has
not executed on the target GPU.

The actual sequence is:

1. Obtain the real callback-issued RetirementScope and GART access. Stop the
   captured inherited HUBP memory readers through their existing blank/drain
   protocol. This does not claim that inherited downstream OTG/DIO stopped.
2. Call Display's `stopConsumers`. A remaining native surface or stream owner
   rejects the call; each must complete its own retirement first. Close owned
   transports and links, stop DTOs and retire owned power domains. For retained
   clock policies, verify physical idle. DMCUB and policy journals stay owned.
3. Stop the real graphics queue, retire VM contexts with queue provenance and
   actual hub detaches, then complete RLC/SRM/clear-state retirement. This last
   step revokes `firmwareReady_`, so it must follow Display consumer retirement.
4. Call Display's `stopFirmware`. Its private FAMS journal selects coordinated
   clock retirement while DMCUB still runs. Only then execute DMCUB STOP_FW,
   PWAIT, reset and runtime-window disable. A reset-only DAL/FAMS owner reports
   Busy with `StoppedRetained`; a stopped firmware report cannot release it.
5. Only after Display releases its lease, retire any remaining uncoordinated
   firmware clock state, PSP allocations, GART mappings and NBIO apertures.
   Disable bus mastering only after those actual retirements succeed.

The no-policy DMCUB-only path retains the old treatment of inherited downstream
display clocks. It does not invent global physical idle from HUBP blanking.
The coordinated bridge still requires its independent actual idle proof.

## Partial startup and failure

Lifecycle may now ask a failed Engine to attempt ordinary retirement once.
Engine admits this only if the actual GraphicsQueue is Running, Firmware is
FirmwareLoaded, GART has both hubs enabled, and Display is absent, Prepared or
Ready. For example, a CPU-only Display factory failure after a successfully
started queue can use the real queue/RLC retirement protocols. Admission itself
does not retire anything. Descendants still perform their own hardware checks.

The private `stopAttempted_` flag is set before the first retirement operation.
It prevents replay after a partial failure; it is never a quiescence flag.
Earlier failures without a running queue, faulted child state, ambiguous writes
and failed retirement still require the existing independently verified full
reset. A stage number or a completed GPU fence does not waive these checks.

This does not close every partial-start case: pre-queue RLC autoload still lacks
an ordinary retirement owner. Ambiguous active clock policies remain retained,
and termination callbacks that prohibit PCI recovery still cannot use a reset
as their terminal policy release. New coordinated startup remains disabled
until those paths and the complete mode/clock transaction are implemented.

## Diagnostics and validation

Each attempted Display phase records bounded historical results, elapsed time,
consumer/firmware completion, runtime and policy retention, lease release and
coordinated clock-retirement result. Engine logs them and publishes separate
`DisplayConsumerRetirement` / `DisplayFirmwareRetirement` observations. These
snapshots contain no hardware authority and are never used for admission.

Initial root validation in `build/engine-display-retirement-local-1` compiled
Engine, Lifecycle and DeviceEngines against real kernel headers with warnings
as errors and the 2 KiB frame limit. Independent Display/Engine reviews passed.
Combined validation in `build/native-engine-display-retirement-final` passed
71,556 ASan/UBSan checks and nine kernel objects; all 105 source inputs were
verified and the 2,956 KiB stage removed. The complete 98-unit kext is
`build/macos-engine-display-retirement-final`; its 245 inputs, six bundle files
and 412 resolved imports were independently verified. No GPU operation is
established by these checks.
