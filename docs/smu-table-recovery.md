# Terminal SMU driver-table reconciliation

The portable implementation is `smuclock::Controller::reconcileMetricsForRetirement`
in `src/smu_clock.cpp`. It is a single retirement attempt for an already faulted
table owner. It never restores normal readiness or clears the native Firmware
fault. The native pre-MODE1 binding is implemented separately by the root lane.
No target GPU execution or startup activation is claimed.

All primary references below are pinned to Linux
`3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`. This is a restricted port protocol
derived from the documented mailbox and synchronous table operations, not a
claim that Linux has an identical scratch-buffer recovery routine or that the
closed firmware implementation has been inspected.

## Source basis and exact boundary

`smu_cmn.c::smu_msg_v1_send_msg`, lines 513-521, polls the previous response
before writing the next command. It refuses response zero and unknown/fatal
responses; known completed or rejected replies permit the next command. The
decoder at lines 249-290 distinguishes success `1`, command failure `0xff`,
unknown command `0xfe`, bad prerequisite `0xfd`, and busy rejection `0xfc`.
Only the new command's own success response proves its success. Unknown replies
put the firmware into a hung state at lines 535-539. A separate response wait
exists at lines 580-601. These rules support bounded late settlement; they do
not turn an expired poll into completion.

`__smu_msg_v1_send`, lines 311-326, clears the response, writes the argument,
then writes the message. Failure before the message write can therefore leave
response zero even though no new command was sent. This implementation does
not invent a skip-prepoll exception to repair that case. It retains the owner
if a real known terminal response cannot be observed within the new deadline.
See the [V1 mailbox implementation](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/pm/swsmu/smu_cmn.c#L249).

`smu_v13_0_set_driver_table_location`, lines 594-612, publishes high then low
address halves. `smu_cmn_update_table_read_arg`, lines 1109-1157, waits for the
transfer and then consumes or reuses its shared scratch allocation. Metrics
reads explicitly request table 5 at lines 1197-1224. These are command-driven
transfers; the driver-table pointer is distinct from the tools/DRAM/STB log
addresses. See [address publication](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0.c#L594)
and the detailed native lifetime audit in `smu-metrics-native-integration.md`.

## Implemented portable API

```cpp
smuclock::Status Controller::reconcileMetricsForRetirement(
    const smuclock::Poll&, smuclock::Result&);
```

There is no caller-provided address, replacement allocation, completion flag,
fault-clear method, or manufactured reset proof. The controller retains its
original allocator pointer, full allocation handle, range and derived MC
address. It reuses that exact page; an additional replacement BO is unnecessary
for this terminal path. Allocation generation/range/address mismatch fails.

The existing `Io.valid` callback must now be bound by the native owner to its
actual pre-reset RetirementScope and installed GART Access. It must establish
the same retained provider, assigned BAR, VRAM allocator, exclusive PPSMC owner,
and uninterrupted firmware/memory epoch. A function argument bool or old
software generation alone is not that authority. The native deadline covers
owner-lock waiting and every callback; the portable deadline also bounds the
whole reconciliation, not each command separately.

The method requires a bound, faulted companion with a possibly published table.
The first valid attempt enters an irreversible private terminal mode. `ready()`
remains false throughout and after success. Reentrant calls and later retry,
capture, publication, range and ordinary seal calls fail before hardware I/O.
Invalid caller poll parameters are rejected before consuming the one attempt.

It then performs this sequence:

1. Validate the same live allocation and actual native authority.
2. Observe a known terminal prior response under the bounded deadline. Record
   its raw value as settlement only. Zero waits; all-ones and unknown values
   fail. Original unknown-response, device-unavailable, changed-firmware and
   backwards-clock faults cannot be reclassified merely by a later success
   value in the response register.
3. Send GetSmuVersion and GetDriverIfVersion, requiring real response `1` for
   each. Require the retained firmware version and exact interface `0x3d`.
4. Revalidate the allocation, republish its full high and low MC address halves,
   and require each command's response `1`. No transfer runs between halves.
5. Submit a new table-5 SMU-to-VRAM transfer and require its response `1`.
6. Perform the real visibility callback and full ordered 392-byte `readVram`
   readback through that retained allocation. Check final allocation, scope
   and deadline before recording completion and sealing the table producer.

The raw read is retirement evidence, not a frequency observation. Its bytes are
not returned as `Metrics`, and no SOC/Fabric observed-range flags are emitted.
Uniform or zero payload values do not replace the actual transfer ACK, nor do
they alone invalidate an acknowledged, completely read transfer's DMA boundary.
The new path issues five commands; every new command requires success `1`.

`Journal::transport` records request and last-message serials, the exact message
and argument, phase, response-clear/argument/message attempts and completion,
and valid response observations. Attempts are recorded before the corresponding
callbacks, with separate callback success. The original fault status/stage is
preserved. Table attempts have their own monotonic serial; reconciliation checks
headroom before starting and associates its completed transfer with its latest
table attempt. Public journal copies provide diagnostics, never authority.

## Memory and common-owner lifetime

No old allocation is released while the prior command is unsettled. Republishing
both halves repairs a partial address update before a fresh transfer; it does
not rely on the unknown mixed pointer being zero. The newly completed transfer
then establishes the explicit DMA boundary. The terminal producer seal prevents
any later command from reusing that retained pointer through this controller.

The table slot can subsequently pass the existing `releaseAllocationsLocked`
guard. The native Firmware object, PSP/TMR, graphics, display, GART, VM and
provider owners remain retained until their own normal or verified-reset
retirement. `faulted()` remains true; the common fault callback is not undone
or repeatedly called as a substitute for a state transition.

SOC/Fabric range history stays independent. Successful reconciliation can clear
table retention while range retention remains true. It neither restores
automatic policy nor allows the ordinary automatic-range API on the faulted
owner. The initial native reset preparation rejects outstanding range ownership.

## Native pre-MODE1 integration

The native path is deliberately narrower than reviving faulted children:

1. Lifecycle obtains the actual RetirementScope, quiesces HDA and performs the
   existing atomic recovery-entry handshake while the provider is still in D0.
2. Before the first MODE1 write, Engine invokes its one-use
   `prepareClockTableForReset(scope)`, installs actual GART Access and passes
   the remaining bounded deadline to Firmware.
3. Firmware holds its own mutex followed by GART's mutex. Its private recovery
   binding validates the real retirement recovery window, provider/resources,
   and deadline around every access. It keeps `FaultedRetained` and
   `gart_->firmwareReady_ == false`.
4. No published SMU resource needs no additional table protocol. A healthy,
   exactly completed table uses the normal terminal seal; an ambiguous faulted
   table uses this reconciliation. Outstanding ranges reject preparation.
5. Only after table retention is closed may the existing MODE1 + ASIC_Init +
   typed retirement-proof sequence handle other retained owners.

Relevant call sites are
`HackyNaviLifecycleController.cpp::retireAfterResetLocked`,
`HackyNaviEngineController.cpp::prepareClockTableForReset`, and
`HackyNaviFirmwareClocks.cpp::prepareClockTableForReset`. These methods are owned
and validated separately from this portable patch. The existing Firmware
allocation guard remains essential even on the verified-reset release path.

This must run before MODE1: the old table-transfer history cannot be carried
across ASIC_Init and treated as an unchanged mailbox epoch. MODE1's debug
protocol at `smu_v13_0_0_ppt.c:2869` contains no observed driver-table pointer
clear. BME-off also does not prove internal VRAM DMA stopped. Neither replaces
the settlement and transfer evidence here.

Generic faulted-child normal retirement remains unsupported. GraphicsQueue's
normal readiness requires FirmwareLoaded and firmwareReady; changing those
flags after table recovery would incorrectly reopen normal consumers. The
chosen pre-reset path avoids that change and uses the already typed full-device
proof for child retirement instead.

## Failure predicates and validation

Every failed or expired reconciliation remains terminal and retains the table.
This includes absent authority; unknown/new epoch; changed firmware/interface;
invalid/reallocated memory; never-settled response; known rejection of a **new**
command; lost access before or after callbacks; partial high/low publication;
visibility or readback failure; backwards time; and deadline/serial exhaustion.
No fallback writes zero to the firmware pointer. No later unrelated query ACK
counts as an earlier transfer completion. There is intentionally no retry after
an attempted reconciliation fails.

The offline tests exercise exact five-command ordering, late ACK, known prior
rejections, both partially acknowledged address halves, response-clear/argument
failures with no message, changed interface/version, reallocated handles,
range independence, reentrancy, and every recovery read/write/revocation/budget
boundary. No production simulation callback is linked into the kext.

`build/smu-clock-recovery-validation.json` records 127,358 passing ASan/UBSan
checks and a real KernelSDK Mach-O compile of the updated portable module.
The earlier `smu-clock-validation.json` remains unchanged. This validation does
not prove target PMFW behavior, establish complete reset recovery for range
constraints, or authorize connecting a startup caller.
