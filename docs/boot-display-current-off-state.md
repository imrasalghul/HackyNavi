# Cold display capture and retirement: current OFF state

Implemented 2026-10-11 after the diagnostic package `45eab551a719` was frozen.
That package and its prior validation evidence are unchanged. This development
follow-up reconciles the narrower capture behavior noted in
`display-power-current-state.md`; it is not a new target measurement.

At pinned Linux commit `3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`,
`dcn32_hubp_pg_control` requests GATE and waits for current PGFSM 0 for ON or
2 for OFF, independently of desired bit 28. Generated DCN3.2 fields place
current PGFSM in bits 31:30, desired state in bit 28, FORCEON in CONFIG bit 0,
and GATE in CONFIG bit 8. Sources:
[sequencer](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/hwss/dcn32/dcn32_hwseq.c#L167-L196),
[register fields](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_2_0_sh_mask.h#L2774-L2813).

`boot_display.cpp` now applies the same exact predicates consistently at
initial admission, request validation, window exclusion, plane decoding and
final OFF-mask publication:

| Current state | Permitted raw STATUS | Permitted CONFIG |
| --- | --- | --- |
| ON | `0`, `0x10000000` | `0`, `1` |
| OFF in posted-cold capture only | `0x80000000`, `0x90000000` | `0x100` |

The ordinary diagnostic `capture` still rejects OFF. Posted-cold capture still
requires the actual zero SOS observations and the caller's full-lifetime
exclusion of competing display/power programmers. Every STATUS/CONFIG/STATUS
triplet and retained raw comparison remains exact, including desired bit 28.
Intermediate or reserved status bits, contrary requests, FORCEON against OFF,
transport failure and all-ones reads fail without creating reservations.
No OFF-window read is added, and no register write occurs during capture.

`boot_display_stop.cpp` already takes its private state through the actual
cold-capture API, compares every retained raw STATUS and CONFIG, and excludes
every captured OFF pipe from HUBP/clock reads and blank writes. Its algorithm
therefore needs no broader permission or masked comparison. Either initially
accepted OFF value is retained exactly; a later change between them faults
retirement and requires retention. Existing active-pipe clock, blank/drain,
ambiguous-write and no-retry rules remain intact.

The native `captureBootDisplay` publisher now recognizes both OFF encodings
when producing `RawSnapshot.Pipes[].WindowRead`. Skipped OFF windows have
`WindowRead=false` and no `RegisterOffsets` or `RegisterValues`. This decision
uses the fully observed raw status, not `poweredOffMask`: a later unsupported
plane format can leave a useful completed snapshot while withholding the
success-only OFF mask and reservation results. Tests exercise that exact
capture condition with each OFF encoding; native OS collection publication
is source-reviewed and needs its separate real-header compile and target run.

Strict C++11 builds and separate nonrecovering UndefinedBehaviorSanitizer runs
passed with 11,554,469 capture assertions and 50,298,906 retirement checks,
zero failures in both configurations. Added cases cover all 16 OFF masks and
16 desired-bit masks, both ON configurations, both OFF encodings, reserved and
request-bit changes, every read failure/all-ones position in mixed and all-OFF
paths, changes between individually accepted OFF values, omitted OFF windows
after format rejection, and ambiguous blank writes on remaining ON pipes.
These host register models are test-only; they do not establish GPU execution,
power transitions, global DMA retirement, memory ownership or acceleration.
Hash-bound evidence is in `build/boot-display-off-current-validation.json`.
