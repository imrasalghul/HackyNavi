# Retirement before the first graphics queue start

This change admits one additional ordinary Engine teardown case: a healthy
native GraphicsQueue whose state is still `Prepared`. It already owns the
real, PSP-backed RLC clear-state allocation. It has never attempted a queue
start. It does not extend retirement to a missing queue, an allocated-only
queue or a faulted queue, and it does not enable any new startup operation.

## Actual authority and order

`gfxqueue::State::neverPublished()` is a read-only predicate over the real
portable queue state. It requires `Fresh`, zero producer and fence sequence,
no pending submission or retention, and no attempted start. The private
start-attempt latch changes before the first start read callback, so even a
failure before DMA publication cannot masquerade as the untouched state.
Invalid argument/layout preflight performs no hardware work and leaves the
predicate true. The predicate alone grants no retirement or release authority.

Under Queue -> Firmware -> GART locks, the Prepared native stop additionally
requires the actual hardware and storage holds, valid provider relationship,
prepared and retained CSIB, the same live allocation owner/serial/slot, a
completed shader upload, no raw/isolated submission history, and an empty VM
registry with no active or failed VM retirement. Only that branch sets the
private never-started-retirement marker and enters `QuiescedRetained`.

The portable queue remains `Fresh`. The native completion-retirement method
rechecks the private marker and all actual owner conditions. With no VM and
no submitted queue, there is no fence to inspect. It neither writes a fence
nor changes the portable queue to `Stopped`.

Before releasing anything, `retireAfterQuiescence` repeats those conditions
and invokes the unchanged real `gfxinit::retireClearState` protocol. That
protocol verifies the exact owned CSIB address/length, current CP/MEC/MES,
ring/VM/selector guards, RLC safe-mode acknowledgement and RLC/SRM/MC/FIFO/
translation drain. Only successful hardware retirement clears CSIB and
permits allocation release. Any ambiguity retains the same owner and buffer.

Engine preserves its existing one-shot stop and original ordering: inherited
scanout and Display consumers, queue/VM retirement, RLC, clock policies,
DMCUB, PSP, GART, then NBIO apertures and PCI bus mastering. Actual callback
RetirementScope and lexical GART access continue to govern every hardware I/O
and the shared callback deadline. No recovery permission is inferred here.

## Remaining pre-queue gap

Firmware loading precedes GFXHUB setup and queue creation. In
`HackyNaviFirmwareController::loadGraphicsFirmware`, completed RLC autoload
is followed by DMUB and register-list loading plus SMU/GFXOFF preparation.
A later queue-factory allocation failure can therefore leave healthy loaded
firmware and retained TMR without any queue/CSIB owner. This patch deliberately
does not call the PSP destroy path in that condition: it would not stop RLC.

A separate Firmware-owned RLC retirement protocol is needed there. Its entry
contract must be resolved from actual ownership and source evidence rather
than constructing a prepared `gfxinit::State`. Linux's PSP path installs CSIB
and enables SRM later in `gfx_v11_0_rlc_resume`; that sequence does not prove
the preexisting CSIB registers are zero. A strict zero-entry profile would be
an additional checked prerequisite, not an assertion about actual hardware.
Unknown/nonzero inherited CSIB ownership and ambiguous autoload/SMU failures
remain outside that proposed first profile. Native PSP/TMR release must only
follow an actual RLC retirement result, not a general `Quiesced` state label
which is also used by clock teardown.

Termination, close and shutdown callbacks still prohibit PCI recovery when
they may hold the PCI workloop. `RetirementScope::enterRecovery` admits only
PowerDown/OwnerFailure reasons. If ordinary retirement fails in a prohibited
callback, the existing terminal fail-stop remains; retaining memory or clearing
BME is not full isolation. Reset-only DAL/FAMS policies also remain unresolved
for those callbacks. This narrow branch does not remove those limitations.

## Source basis and checks

The source pin is Linux `3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`:
`gfx_v11_0.c:2306-2377` supplies CSIB, RLC stop and SRM programming;
`:2522-2531` shows the PSP CSIB/SRM sequence; `:5445-5463` supplies RLC safe mode.
The existing port adds explicit hardware drain checks before releasing CSIB.
`amdgpu_psp.c:3581-3590` and `:3902-3914` establish that autoload is a distinct
PSP command, earlier than graphics queue initialization.

Focused tests use the actual portable queue state machine and existing offline
MMIO/PM4 model. They inspect the predicate from start callbacks, fail every
start register access, reject every attempted/faulted/stopped queue, and retain
the untouched state through invalid preflight. They also execute the real
portable prepared-CSIB retirement without ever starting a queue, including
every failed read or possibly landed write and repeat-attempt rejection.
These checks are protocol tests. Native objects are compiled against actual
kernel headers; neither check executes a GPU or a native IOKit object.
