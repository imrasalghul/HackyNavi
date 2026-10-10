# DCN3.2 boot force-on observation

Target evidence: `E:/HackyNavi-diagnostics-20261010-212355/device.txt`,
BootDisplay from capture `1e4040229efa`. DOMAIN0_PG_STATUS at BAR5 byte
`0xd504` returned `0x10000000`. The callback succeeded; the probe validity
mask was `1`. CONFIG and the confirming STATUS were **not read**. Their
zero fields were placeholders, not observations. Capture stopped after SOS
and the first power status; it did not enter the HUBP window.

The discovery DMU BASE_IDX2 is `0x34c0`; the generated DOMAIN0 status offset
is `0x81` DWORDs, giving `(0x34c0 + 0x81) * 4 = 0xd504`.

## Source findings

All Linux references use commit
`3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`.

- `DOMAIN_DESIRED_PWR_STATE` is bit28; `DOMAIN_PGFSM_PWR_STATUS` is
  bits31:30. Thus the actual word has current PGFSM0 and desired1, without
  reserved bits. It is not the PGFSM1/3 encoding. Local source:
  `.research/linux/dcn_3_2_0_sh_mask.h:2774–2813`.
  [Generated fields](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_2_0_sh_mask.h#L2774-L2813).
- Linux writes POWER_GATE0 for on and1 for off, then waits only for
  PGFSM0/2 respectively. It does not use desired1 as a standalone reason
  to declare the block unreadable. Local `.research/linux/dcn32_hwseq.c:167–196`.
  [HUBP power control](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/hwss/dcn32/dcn32_hwseq.c#L167-L196).
- Linux uses FORCEON1 to disable power gating. It also explicitly documents
  FORCEON1 as the hardware default at boot and S3 resume. These provide the
  basis for a conditional force-on read policy; they do **not** establish
  that this target's still-unread CONFIG actually contains1. Local
  `.research/linux/dcn32_hwseq.c:137–164,866–870`.
  [Force-on control](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/hwss/dcn32/dcn32_hwseq.c#L137-L164),
  [boot default](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/hwss/dcn32/dcn32_hwseq.c#L866-L870).

## Implemented policy

The reader now allows raw `0x10000000` to reach the always-on CONFIG read.
It enters a HUBP read window only if CONFIG is exactly `0x1`, meaning
FORCEON1 and POWER_GATE0, and the confirming status is identical. Existing
per-window and whole-pass checks continue to require identical raw status
and CONFIG. This is a source-conditioned admission based on the actually
observed override; it does not assume an override from the target status.

CONFIG0 remains rejected for that status because it supplies no force-on
override. CONFIG`0x101` remains rejected because accepting it would require
an additional policy for precedence over an explicit gate request. Unknown
bits, PGFSM1/3, incomplete off encodings, all-ones, failures and changing
observations remain rejected. Existing on0/config0-or1 and cold completed
off`0x90000000`/config`0x100` cases are unchanged.

`inventoryPower` independently reads STATUS/CONFIG/STATUS for all four
domains, with at most12 reads. It records raw values and per-read validity
without interpreting arbitrary or changing states as HUBP permission. It
stops globally on callback failure or all-ones. It never touches SOS, VGA,
HUBP, cursor, metadata, or framebuffer/global windows and has no write
callback. Ok means only that all12 raw reads completed without all-ones.

## Validation and limits

Local ASan+UBSan boot-display tests: **4,568,185 assertions, zero failures**.
They cover exact status/config combinations for every pipe and both capture
entrypoints, real forced-on-branch simulated surface decoding, every one
of801 read failures/all-ones, loss of FORCEON, a late gate request, reserved
bits, changing status, raw all-four inventory ordering, every inventory
failure phase, and malformed layout preflight. All tests use explicitly
simulated callbacks; no hardware was accessed. Production uses real native
callbacks and has no mock fallback.

The target still needs a new real capture to determine its CONFIG and actual
scanout. This change adds no display writes, allocator authority, coherent
snapshot guarantee, modeset, engine activation, Metal, or retirement proof.
Existing external exclusion requirements remain mandatory. Other owners'
`display_power` and `display_idle` classifiers were not changed; later
activation must reconcile their narrower admission separately. The separate
VCN0 all-ones observation is not explained or repaired by this change.

Native integration/build verification is parent-owned. Frozen source hashes
and local validation command are in `build/boot-display-forceon-validation.json`.


## CONFIG0 target evidence and correction (2026-10-11)

This section supersedes the earlier FORCEON-only admission policy. That
policy was conditional on an unread CONFIG value, not a verified description
of this target. The new actual capture at
`E:/HackyNavi-diagnostics-20261010-232322/device.plist`, from immutable build
`dbbcb5ec0ba9`, records all four domains with STATUS/CONFIG/STATUS equal to
`0x10000000 / 0x00000000 / 0x10000000`. Each validity mask is 7. The independent
inventory completed all 12 reads; its last register is BAR5 byte `0xd51c`.
The separate classifier then rejected CONFIG0 at `0xd500` after SOS, STATUS,
and CONFIG; its mask 3 correctly means its confirming STATUS was unread.
No HUBP snapshot or display surface was captured on that boot.

The actual CONFIG disproves the earlier assumed need for a FORCEON override.
Pinned Linux `dcn32_hubp_pg_control` sets CONFIG0 for power-on (FORCEON0,
GATE0), then waits only on current PGFSM bits31:30 being0. It does not require
desired bit28 to clear. See `.research/linux/dcn32_hwseq.c:167-196` and
[the pinned source](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/hwss/dcn32/dcn32_hwseq.c#L167-L196).
Generated fields at `.research/linux/dcn_3_2_0_sh_mask.h:2774-2813` confirm
that desired and current fields are distinct. This supports admitting a
stable current0/desired1 observation with either CONFIG0 or CONFIG1; it does
not identify why firmware leaves desired1, or promise future power stability.

The reader now applies the same CONFIG0/1 rule to both known current0 words
(`0x00000000` and`0x10000000`). Explicit GATE requests, FORCEON+GATE, unknown
bits, current PGFSM1/3, transport errors, all-ones and changing raw samples
remain rejected. The complete cold OFF case remains
`0x90000000 / CONFIG0x100`. No power-control writes, access bypass, allocator
authority or modesetting were added. Every window guard still compares the
entire retained status/config pair, and both complete passes must satisfy the
existing stability policy before `snapshotReadComplete` is set.

Tests now reproduce the four actual power pairs while using explicitly
simulated scanout contents (the target has not supplied those contents yet).
Both capture entrypoints complete on CONFIG0/1; format rejection retains a
complete raw snapshot. Every read position is tested for failed/all-ones
callbacks, and every power guard for a new gate request, unknown bit or
intermediate current state. CONFIG changes between independently acceptable
values remain checked: a new value before its first observation is data;
changes at later guards or between passes reject the classification.
Retirement tests use the actual capture API to obtain its private state and
cover both CONFIG values, mixed OFF/blank/ON pipes, every read failure,
changed retained power evidence, and partial write failure. No production
`boot_display_stop` change was needed.

Strict C++11 (`-Wall -Wextra -Werror`) with ASan+UBSan passed locally:
6,592,514 boot-display assertions and 37,457,089 retirement checks, zero
failures. These are host tests, not execution on the target GPU. Commands,
output and frozen source hashes are recorded in
`build/boot-display-desired-state-validation.json`.
Native compilation and the next capture package are parent-owned.

The other production power owners (`cold_dma`, `display_idle`,
`display_power`) remain unchanged and need whole-protocol reconciliation
before activation. In particular, changing only `display_power::settled`
would be incorrect: its raw-zero checks also decide HUBP access, whether to
transition, and completion polling. The VCN0 all-ones observation is separate.
A new target capture is still needed for actual HUBP/scanout register evidence;
this correction alone does not establish successful display control or Metal.
