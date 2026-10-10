# Native PPSMC metrics ownership and retirement

This is a source audit and implementation contract, not a claim of target
execution. It supplements `display-clock-hold-design.md`. The portable
`smu_clock` companion and native FirmwareController adapter are now implemented
and linked, with Engine teardown connected. Normal request/capture activation
remains separate. See [implementation and validation](validation-smu-clock-owner.md). All Linux references use commit
`3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`. Line references below refer to
the local, pinned copies under `.research`.

## Actual existing owner

`HackyNaviFirmwareController` owns the sole initialized PPSMC `smu::State`
(`.hpp:99-101`), retains GART, and holds GART's unique `firmwareOwner_` lease.
`loadGraphicsFirmware()` initializes that state after PSP/autoload and disables
GFXOFF (`.cpp:246-283`). The new companion must borrow this state and use the
same Firmware -> GART locks. It must not create a second SMU initializer or
reuse the first-message response-poll exception. `smu::verifyAwake()` is a
cold-start check involving idle CP state; it is not a runtime metrics gate.

The existing native PPSMC calls are version/interface queries and GFXOFF
control. A search of `kext/HackyNavi`, `src`, and `include/hackynavi` found no
other native driver-table address or table-transfer producer. The new portable
`smu_clock` draft is the only added table producer. The separate `early_media`
protocol sends power commands and is not connected to native activation.
DAL in DisplayController is a separate mailbox with a different command map;
it must not become an alternate PPSMC owner.

Normal access requires the actual FunctionGroup Operation and installed GART
Access. Retirement requires the actual RetirementScope and corresponding
GART Access. Under these, Firmware's `providerReady()` checks the retained
provider/open owner, assigned BAR, PCI identity/BME, GART state, HDP state,
and lease (`.cpp:99-116`). Start one elapsed deadline before waiting for the
owner locks and check it around every callback. The companion's own per-call
timer does not include an earlier native lock wait.

## Native API and allocation

An initial internal API can be:

```cpp
IOReturn captureSmuMetrics(HackyNaviFunctionGroup::Operation&,
    uint32_t timeoutUs, MetricsObservation&);
```

`MetricsObservation` contains a private-owner-issued generation and capture
serial, raw 392-byte external metrics record, decoded 13 current clocks and
counter, and diagnostic result. It grants no fixed-clock or DML authority.
A separate typed SOC/Fabric range request may use the same owner, but metrics
buffer retirement must not silently release or forget those constraints.

Implementation belongs in FirmwareController, beside its existing private
`smu_`, with a private `smuclock::Controller` and dedicated `VramAllocation`.
Allocate a 4096-byte-aligned, 4096-byte VRAM slot through `gart_->allocator_`.
Derive the command address through `allocator_.addresses(handle, mc, physical)`
and publish **mc**. The allocator's other output is a GPU PTE physical address,
not the CPU BAR address. Do not accept a caller-supplied address, bytes, DMA
descriptor, or allocation-completeness assertion.

The exact interface is `0x3d`; table 5 uses `SmuMetricsExternal_t`, 392 bytes,
including the external padding. `CurrClock[13]` begins at zero and the metrics
counter is at byte 80. Sources: `clock-hold/smu13_driver_if_v13_0_0.h:27,
439-453,1424-1491,1611` and `linux/smu_v13_0_0_ppt.c:480-485,727-768`.
Navi31 running-feature commands are low `0x0c` and high `0x0d`, both parameter
zero (`smu_v13_0_0_ppt.c:120-142`; `smu_cmn.c:811-845` takes its separate-low/high
fallback for this map). They are not the generic single-message, parameter-0/1
variant used by some other ASICs.

Call `holdHardware()` before the first potentially delivered address command.
It is already idempotent (`FirmwareController.cpp:130-132`). Keep the slot,
owner and GART retained if any relevant command may have landed. The native
fault callback must latch `FaultedRetained` and clear `gart_->firmwareReady_`
while its locks are held. Do not call existing `fault()` inside a callback:
that method also unlocks both owners (`.cpp:122-129`). The outer operation
must perform exactly one unlock and return its detailed protocol failure.

## Readback and lock order

After the transfer's actual response 1, use the existing ordered native
`beforeReadback()` approach (`FirmwareController.cpp:145-153`), then
`readVram()` over the retained allocation. This reads through the uncached
MM_DATA window; it also writes MM_INDEX_HI/MM_INDEX, so keep the GART lock
for the entire transfer and readback. Clear partial output on any failure.
Never use the system-DMA `synchronizeForCPU(Region)` path on a fake descriptor.

The prior design note's generic HDP-invalidation wording needs this ASIC
qualification: Linux's `amdgpu_hdp_invalidate()` only dispatches an optional
callback (`amdgpu_hdp.c:70-75`), and HDP6's function table has **no invalidate
callback** (`hdp_v6_0.c:129-133`; selection in `amdgpu_discovery.c:3539-3544`).
Do not invent an invalidate-register write from a generated register name.
The current indirect, uncached readback needs ordering and actual scope
checks. See [HDP6 implementation](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu/hdp_v6_0.c#L129).

The hierarchy is actual FunctionGroup operation -> Engine/Lifecycle as
applicable -> Display, if held -> Firmware -> GART. A Display method which
already holds Firmware/GART must use a private locked helper, not reenter a
public Firmware method. An outer coordinator can instead make sequential
Display and Firmware calls within one real operation. Never introduce a
Firmware -> Display acquisition to obtain quiescence.

## What a transfer ACK proves

Linux sets the driver-table address with high then low commands
(`smu_v13_0.c:594-612`). `smu_cmn_update_table_read_arg()` uses that single
scratch allocation for different table types, waits for synchronous command
completion, then immediately consumes SMU-to-DRAM data or reuses the scratch
for a subsequent command (`smu_cmn.c:1109-1157`). Navi31 I2C likewise consumes
its response in that shared scratch after its synchronous update
(`smu_v13_0_0_ppt.c:2683-2749`). Metrics reads explicitly issue table 5 whenever
the cache is bypassed or expires (`smu_cmn.c:1197-1224`). These paths establish
completion of the explicit transfer before host use/reuse.

The driver address is not a periodic metrics stream in this protocol. Tools,
DRAM log and STB log have different address commands (`smu_v13_0_0_ppsmc.h:
52-58,73-75,122-125`). No source found enables periodic metrics DMA through the
driver-table pointer. This conclusion applies to the exact interface and
restricted commands audited here, not arbitrary SMU services. See the
[synchronous transport](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/pm/swsmu/smu_cmn.c#L1109)
and [Navi31 I2C consumer](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0_0_ppt.c#L2683).

## Implementable normal close

A successful explicit transfer need not force whole-device reset merely to
free this scratch. The following is a restricted port contract inferred from
that synchronous transfer/reuse protocol. Linux itself normally keeps the
scratch BO across hardware fini/reset and frees it during software teardown;
it does not free the BO after every metrics call.

1. Under the actual normal/retirement scope and Firmware -> GART locks, close
   admission to this owner's future table-address and table-transfer commands.
   This must be an irreversible private state transition, not a caller boolean.
   Drain is provided by the same exclusive locks and outer operation gate.
2. For a published slot, require both address halves acknowledged, the latest
   attempted transfer acknowledged, and its ordered complete readback. Require
   the full allocation handle/range/MC address still match. A transfer result
   from before a later attempted transfer or address write is not sufficient.
3. Bind the close record internally to the actual Firmware instance, FunctionGroup
   generation, allocation generation, and latest table-attempt sequence. Advance
   that sequence **before** each potentially delivered address/transfer command;
   overflow fails before writing. Keep it separate from the successful capture
   serial. No externally supplied POD may manufacture a retirement proof.
4. Consume this record under the same locks to release only this scratch slot.
   Future table operations on this closed owner fail before any write. A new
   owner must publish both halves of a retained new allocation before its first
   transfer. No speculative zero-address command is needed or supported.

Generation matching differs between these two native call paths. A normal
close requires the live Operation generation to equal the capture's generation.
`FunctionGroup::requestRetirement()` advances generation before creating its
RetirementScope (`HackyNaviFunctionGroup.cpp:667-668,693`); a retirement close
therefore preserves the old acquisition generation as provenance and binds the
same retained owner/allocation to the actual new RetirementScope. It must not
require those two generations to be equal, or relabel the old capture as a fresh
observation. Only the native retirement owner can establish this transition.

This proves the completed explicit DMA reference has no future producer in
this driver. It does **not** claim that the firmware pointer is cleared, that
all SMU DMA is stopped, that SOC/Fabric constraints are withdrawn, or that PSP,
DMCUB and graphics allocations can be freed. Keep those retirement predicates
independent. No-publication factory cleanup can simply release its unexposed
slot. Successful publication and capture should be one native entry operation,
so the normal successful return already has a closeable latest transfer.

On ambiguous publication/transfer, latch the common owner fault and retain.
A later query ACK cannot substitute for the missing transfer completion.
Normal close must not create a fake completion to avoid retained failure.

## Address replacement and failure recovery boundary

A complete high/low publication is a supported operation; partial high/low
publication can leave an unknown mixed pointer. It does not itself request a
table transfer. Replacement at a known completed transfer boundary is a
reasonable extension of the serialized command protocol, but no runtime
scratch-BO replacement/free example was found in the pinned driver.
The first implementation should therefore keep one allocation for its owner
and close it terminally as above, without adding live replacement.

For any future explicit reconciliation API, retain old and replacement BOs,
first establish that the previous command has actually settled, acknowledge
both halves of the full replacement address, then acknowledge a transfer to
the retained replacement and complete its ordered readback. No table command
may run between half-address updates. Only then can an exact journal authorize
old-slot release. A permanently faulted companion cannot simply be rebound or
have its flags cleared to implement this; that requires a separate reviewed
protocol and common-owner recovery transition. Until then, failed publication
remains retained through the actual recovery owner.

## Native teardown sites that must change before activation

`FirmwareController.cpp:232-245` currently releases all private slots without
an SMU-table predicate. Both `retireAfterQuiescence():298-328` and
`releaseAfterDeviceReset():329-345` reach it. `free():346-360` relies on the
hardware self-retain preventing destruction after publication. Adding a metrics
slot without the close guard would silently weaken all three paths.

Normal `retireAfterQuiescence()` already requires queue/Display consumers gone.
Before releasing allocations, consume the private completed-table close record
and independently resolve any clock-range ownership. An outstanding range
cannot be discarded merely because its metrics slot closed. For a metrics-only
owner with a successful final capture, normal close above needs no reset.

Engine's `releaseAfterVerifiedReset():450-484` has the actual typed proof and
provider binding; it presently calls Firmware's untyped reset release at line
476. Any additional reset-dependent SMU retirement must receive and validate
that real proof, not an enum or bool. However, the existing reset observations
do not explicitly observe driver-table pointer clearing or SMU table transfer
completion. MODE1 alone must not be relabelled as that evidence.

Linux's ordinary unload cancels SMU work, disables applicable power activity,
and issues `PrepareMp1ForUnload`; reset/suspend paths deliberately differ
(`clock-hold/amdgpu_smu.c:2143-2213`; `smu_v13_0_0_ppt.c:2821-2842`). Its BO is
freed in `smu_fini_fb_allocations():1071-1088`, called from software teardown.
These are future whole-SMU terminal-policy references, not authorization to
issue unload while native Firmware consumers remain. See
[SMU lifecycle](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/pm/swsmu/amdgpu_smu.c#L2143).

This audit itself performed no target test. Native scope/lock compilation,
close and failure-injection tests, independent review and full linking now pass
as recorded in [the implementation validation](validation-smu-clock-owner.md).
No normal capture/range caller is activated; ambiguous-table recovery and the
complete display transaction remain separate requirements.
