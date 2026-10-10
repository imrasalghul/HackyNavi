# Native lifecycle containment: integration design

Status: the normal gate, callback retirement worker/fail-stop policy, Device
registration/activation and descendant per-I/O scope propagation are implemented
and native-linked in `build/macos-device-engines-final`; its 364 imports resolve.
The later client/model-input integration links 85 units and 406 imports in
`build/macos-client-display-integration`, including normal completed-VM retirement.
Target execution is still pending. The immutable capture package does not publish DMA. The
development Device's additional `-hackynavi-engines` opt-in invokes real engine
startup under actual paired PCI ownership. Its explicit audio stop does not
isolate a PCI bus or veto parent power loss.

## Implemented callback retirement

Before first-attach admission, the actual normal Operation registers its
primary owner's retirement handler and a total monotonic deadline (1ms–5s).
Both callouts are allocated before registration. Down/termination/shutdown/close
callbacks revoke admission before waiting and join one terminal transition.
A KERNEL-priority worker drains the operation gate, then invokes the owner with
a noncopyable RetirementScope. Concurrent callbacks share the first deadline
and generation. Only actual successful retirement permits ACK/close; error or
timeout explicitly panics while retaining potentially live owners. A HIGH
watcher uses atomic state and no hardware/owner locks as an independent check;
it is not a hard realtime guarantee.

The scope permits cleanup on the exact retained pair and worker thread, with
live PCI/D0/open/topology validation. Logical owner/lease termination is allowed
for cleanup; actual PCI disappearance is not. CPU-only unpublished cleanup
does not require readable hardware. Normal admission is permanently revoked
after this transition; a power-on callback cannot rearm it.

MODE1/PCI restore requires `enterRecovery()`, a single atomic transition shared
with terminal reason publication. Only a nonempty subset of ordinary PM-down
and OwnerFailure may enter it. OwnerFailure is admitted only from the exact
owner's registering thread after its normal Operation ends; Device calls this
immediately on the same worker invocation after failed activation. A racing
terminal callback after recovery entry fails immediately rather than waiting
on a worker potentially dependent on the termination workloop. Lifecycle now
uses that API before reset. Device now publishes the Lifecycle before binding
its handler, keeps its own PM/termination callbacks pending, and prevents
unmap/close until success. Primary retirement permanently revokes admission
before checking registration and drains/rechecks an unarmed registration race.

Each queued callout owns a group retain. Cancellation drops the watcher retain
only when cancellation succeeds; otherwise the watcher releases it. Free does
not wait on its current callout. Evidence:
`build/native-callback-retirement-final/report.json`. Compilation proves neither
callback ordering on this machine nor a successful hardware retirement.

## Implemented normal-operation gate

`HackyNaviFunctionGroup::beginOperation` now issues a noncopyable, thread-bound
RAII scope holding a real shared `IOLock`. It freshly validates both function
identities, actual D0 through a bounded conventional-capability walk, open
ownership, topology and the generation before admitting work. Per-I/O `valid()`
observes immediate atomic revocation without waiting for either callback lock.
Both functions publish revocation before joining the operation gate and return
their PM acknowledgement after the previous CPU operation has released it.
Shutdown publishes a permanent admission stop before waiting for any lock.
The scope retains the group, unlocks before releasing it and rejects same-thread
nesting. Programming-contract violations on same-thread shutdown/transition
or cross-thread destruction are explicit failures, not DMA containment.

The order is operation gate, group-state lock, then individual lease-state
lock. A callback retains the group under its lease lock, releases that lock,
then waits for the operation gate. Notifications run outside the gate and do
not revoke a newly admitted generation after the transition has completed.
Never wait for PM, close/terminate a provider, or synchronously enter an
inherited/configuration workloop while holding a normal scope. XNU dispatches
termination through the victim's workloop; its dependency on this target's
PCI configuration workloop remains unverified. In particular,
`restoreDeviceState` belongs in a separately audited retirement path.

This gate drains CPU operations only. A submitted ring or DMA engine may
continue after a scope ends. The implemented callback worker is bound to the
actual Device owner in the separate engine opt-in. GART, firmware, queues,
VM, display and reset/ATOM callbacks require the installed actual scope before
and after I/O. Stack guards retain their controllers and clear borrowed scope
pointers before releasing them. No scope binding escapes the lexical normal
operation or retirement callback. The Device still advertises no acceleration.

The worker tracks a wake serial for both paired-function notifications and
primary power-on. It requeues a wake received while a prior invocation was
pending, including before the pair exists. Creation owns group publication;
the Acquired callback can run before the factory returns and cannot adopt its
borrowed pointer. Heavy lifecycle work and retirement waits occur outside the
Device mutex. Actual startup is one-shot, with no power-on retry or first-attach
reissue after a fault. Failed activation joins OwnerFailure only after the
normal Operation destructs. No real callback ordering or GPU execution is yet
established by target evidence.

## Native audio stop

`quiesceAudio(Operation&, timeoutUs, report)` uses the still-held real group
scope to access the exclusively owned function 1. It checks identity, D0,
BAR0 assignment and deadline around every configuration/MMIO access. Its
bounded HDA protocol enumerates GCAP streams, stops their RUN bits and command
rings, disables position DMA, verifies the results and holds controller reset.
It then clears BME with readback and samples PCIe Transactions Pending until
clear or the shared deadline expires. The latter covers nonposted requests;
it is not a posted-write drain or a whole-GPU retirement proof.

The adapter directly retains the assigned IOMemoryDescriptor from the existing
device-memory property and maps
that assigned range uncached. It avoids IOPCIDevice's convenience accessor,
which can call `setTunnelL1Enable` and enter the configuration workloop while
the operation gate is held. The stack mapping owns no DMA allocation; releasing
it after all CPU callbacks return neither frees an inherited buffer nor
establishes safe retirement. Errors preserve the first result and any attempted
write. No caller may free DMA based on this report. Engine's first-attach path
now calls it through the real Operation before other control writes; Device
still does not invoke Engine startup. Construction and diagnostic capture do
not call it. The retirement-scope overload uses the same actual adapter.

`IOPCIBridge::constructRange` creates IOMemoryDescriptor instances; the legacy
IODeviceMemory return type of its convenience getter does not guarantee that
subclass. The native symbol check caught the unavailable IODeviceMemory
metaclass, and the adapter now validates the actual base descriptor type.

## What the native APIs actually guarantee

The pinned [XNU 11417.140.69 IOServicePM.cpp](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/iokit/Kernel/IOServicePM.cpp)
dispatches `driverSetPowerState()` from a thread call, outside the PM workloop's
action. After the driver returns, `pmDriverCallout()` enters the PM workloop to
complete that action. This permits sleepable work in the callback; it does not
make recursive PM operations or arbitrary workloop dependencies safe.

`notifyControllingDriverDone()` treats a negative `setPowerState()` result as an
acknowledgement. A positive result requests a timed delayed acknowledgement.
`ack_timer_expired()` can advance the state machine when a third-party driver
misses that deadline. Returning an error, delaying the ACK indefinitely, or
retaining an object is therefore not a way to prevent power removal. The
2-second `pmcallouttimer` is a diagnostic log timer, not a reliable isolation
deadline. A separate system sleep watchdog can also intervene.

`PMstop()` inhibits future normal driver callouts and requests asynchronous
power-plane detachment. Its `waitForPMDriverCall()` uses a 15-second wait and can
time out. Consequently our terminal flags, callback ownership and access drain
must work independently of PMstop completing a drain. Never hold a mutex needed
by callbacks while invoking PMstop; do not invoke it from the retirement callback.

The pinned [IOPCIFamily 681.140.3 IOPCIDevice.cpp](https://github.com/apple-oss-distributions/IOPCIFamily/blob/IOPCIFamily-681.140.3/IOPCIDevice.cpp)
delegates ordinary open arbitration to IOService's single-owner implementation.
An ordinary kernel client's `close()` does not disable bus mastering or clear a
permanent configuration shadow. Those additional close actions are specific to
the DriverKit session path and must not be assumed for this kext. The group
uses no seize option and refuses an existing active audio client.

## Retained memory is not hardware isolation

Keeping the DMA buffer, IODMACommand, map, PCI nub and their owners alive avoids
this driver explicitly freeing them. It neither stops the device nor establishes
that parent teardown preserves its DMA address space. BME-off readback and the
PCIe Transactions Pending observation are useful evidence, but do not establish
retirement of internal VRAM readers, scanout, RLC, PSP, or every translation and
writeback client. They must not manufacture a `RetirementProof`.

`HackyNaviResetController::verifyRetirement()` checks these additional hardware
clients after MODE1 and the actual ROM's ASIC_Init. That typed proof, or the
complete ordinary stop path, is the current route to releasing published owners.
An API return code or a requested reset is not equivalent evidence.

The pinned [IOPCIBridge.cpp](https://github.com/apple-oss-distributions/IOPCIFamily/blob/IOPCIFamily-681.140.3/IOPCIBridge.cpp)
also rules out assuming a generic emergency isolation primitive. `resetDevice()`
enters the PCI configuration workloop and can affect every child below a bridge.
Warm reset requires platform support; the base `warmResetDisable()` implementation
panics. Hot reset deasserts reset and can restore configuration afterwards. None
is a portable, automatically verified, permanent isolation API for this target.
Use requires a separate audited platform implementation and the whole affected
device group, rather than a guessed successful fallback.

## Executable control flow to implement before activation

1. Bind graphics and audio clients, complete their actual PM-on callbacks, and
   capture the FunctionGroup epoch. Check inherited audio-engine inactivity with
   a real audio-controller protocol before using a device-wide reset. The group
   open by itself proves neither audio-engine inactivity nor firmware exclusion.
2. Use a dedicated sleepable lifecycle worker. All submission, new mappings and
   engine startup share one owner admission gate. PM and termination callbacks
   atomically revoke new admission before waiting for that gate. Native MMIO and
   submission entry points must consult this real callback-owned epoch, not an
   independently supplied readiness flag.
3. On an ordinary down transition, keep the PM callback in flight. Obtain a
   **callback-scoped retirement window**, distinct from the revoked normal-work
   epoch. It must bind the group instance, transition generation, callback
   lifetime and live open/D0 identities. It expires before the callback returns.
   It authorizes cleanup only, never new application submissions.
4. Exactly one worker owns retirement for that transition. It executes ordinary
   stop where permitted, followed at most once by MODE1 -> native ASIC_Init ->
   hardware verification -> typed owner release. It retains every published
   allocation throughout failure and recovery. Concurrent graphics/audio down
   callbacks join this same completion; neither returns an ACK early.
5. Success requires the real hardware-retention fields to be clear. A successful
   reset can separately leave `RetiredShadowMaintenance`: Lifecycle refreshes
   and verifies the permanent PCI shadow inside the same recovery-authorized
   scope before accepting success. The terminal group permits no later normal
   activation/config mutation. Do not conflate this retained obligation with live DMA, or assume
   an unsupported "clear permanent shadow" operation.
6. An unpublished failure with no retained hardware may clean up normally. A
   published failure without verified retirement/isolation must take an explicit
   terminal path before PM ACK, PCI close, BAR unmap or DMA-domain release.
   For opt-in experimental engine bringup, the implemented terminal path is a
   deliberate fail-stop panic that retains the owners and records the exact
   transition, stage and return codes. Panic does not prove the GPU
   stopped; it prevents normal execution from deliberately continuing through
   an unproved release. Do not issue a fabricated successful retirement result.

Surprise removal is a different branch: once a provider is inactive or its
identity/power is unavailable, do not access stale BARs or attempt MODE1 through
them. If published owners remain, use the reviewed terminal policy. If none
remain, the ordinary no-DMA teardown is sufficient. There is no error-return PM
veto in either branch.

## Avoiding deadlock and timeout races

The FunctionGroup notifier remains observation-only. With a retirement handler
registered, the actual callbacks first wait for retirement and only then notify/ACK.
Its typed epoch is also an observation, not transaction exclusion: power state
is sampled across two independently locked clients, and can change after a
successful validation. Immediate per-client atomic revocation tightens rejection
during a callback but cannot close that final time-of-check/time-of-use window.
The common operational gate must include power callbacks and every open/close
actor as well as MMIO, submission and allocation release. Generation numbers
alone are not a substitute for that serialization.
Queueing retirement and immediately returning from a down callback is incorrect.
Normal epochs intentionally become invalid at power-down; using one for recovery
would also be incorrect. The implemented callback-scoped retirement window
supplies that distinct authority; Device integration must preserve it.

A practical integration uses a dedicated worker and condition variable. PM
callbacks post/join a retirement request, then sleep with the owner mutex released
until verified completion. The worker never waits for those callbacks to return.
It must not request another power state, join/detach a PM tree, call PMstop,
terminate a service, or synchronously close the function-group clients while their
callbacks are waiting. Event notification holds neither a group nor lease mutex.
Shutdown and detach happen later, after successful retirement and callback drain.

The native reset's `saveDeviceState()`/`restoreDeviceState()` paths need special
attention: the latter enters `setDevicePowerState(...Doze, On)`, performs a PCI
configuration operation, and can invoke registered configuration handlers.
`IOPCIBridge::restoreDeviceState()` itself returns success even when inner restore
work fails, which is why the native reset already performs fresh readback checks.
For the first active profile, reject tunnels, bridge-wide reset, and foreign
configuration handlers; audit the exact native path for reentry into the waiting
owner and for PCI configuration-gate dependencies. Do not invoke the worker while
holding that gate or the PM workloop. A clean callback thread alone is not proof
that all downstream operations are free of these dependencies.
`setConfigHandler()` can return the previous handler, but the examined
implementation also overwrites the handler even when its replacement is null.
Do not treat that method as a harmless read-only query or silently remove a
foreign handler to satisfy an exclusion check.

Use one monotonic deadline for retirement, including waiting for an in-progress
startup, normal stop, reset, ASIC_Init and proof transfer. Existing per-operation
poll limits do not bound a mutex deadlock or their cumulative wall time. A separate
watchdog must observe a minimal atomic transition/stage record without taking the
possibly stuck lifecycle mutex, and execute the reviewed terminal policy before
any delayed-ACK expiry could proceed. Select and validate the deadline against
the target PM watchdog and measured hardware operations; do not invent a timeout
as a successful power veto. The independent HIGH-priority watchdog and timed
callback wait are now implemented; their target behavior remains unverified.

Required target evidence includes both functions' PM registration and exclusive
ownership, real fence/shader execution, ordinary retirement, concurrent down
notifications, and recovery failure that cannot reach the release/ACK path.
Compile/link success establishes none of these runtime properties.
