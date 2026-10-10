# Additional firmware display context capture

`bootdisplaycontext::capture` adds a separate, bounded read-only inventory. It
does not modify the existing framebuffer classifier, admit TLB0, skip an unused
pipe, create reservations, or enable any engine. The completed 3fa target capture
answers the timing questions left by the successful `9eda` boot.

The native implementation is built into
`build/releases/display-context-20261011/HackyNavi-live-capture-3fa3f6d4723b.zip`
(2026-10-11, 16:05 America/New_York). Its adjacent OpenCore agent prompt carries
the exact hashes and test instructions. The original `9eda` is the target-tested
rollback. The new package has source, binary and manifest review in
`build/vertex-context-native-build-review-1` and
`build/vertex-context-package-review-1`. The exact binary has now loaded and
returned both complete, unchanged context passes. See
[target result](validation-2026-10-11-3fa-context-success.md). The next requested
boot must attempt graphics; no further context-only capture is to be issued.

The native caller first requires the existing BootDisplay snapshot to have
completed. It must then keep its actual mapping, PCI/D0 state, generation and
diagnostic worker lifetime valid, serialize its own teardown/reset/power work,
and implement `Io.valid` using those current facts. The portable capture rechecks
that callback around successful reads and guards every HUBP-window access with
fresh power-status/config/status reads. These checks cannot exclude firmware or
another display programmer. Data remains historical and carries no hardware
ownership or reservation authority, even if both passes match.

There are 32 additional register words per pass, all from discovery BASE_IDX2.
At the target's observed base `0x34c0` DWORDs, the following addresses apply;
the implementation always resolves discovery and checks BAR bounds rather than
hard-coding those byte addresses.

| Group | DWORD offset / instance stride | Target pipe0 byte offset | Purpose |
| --- | --- | --- | --- |
| VTG_CONTROL | `0x52e / 1` | `0xe7b8` | VTG enable bit31 |
| OTG_CONTROL | `0x1b41 / 0x80` | `0x14004` | Requested master bit0, current master bit16 |
| OTG_CLOCK_CONTROL | `0x1b85 / 0x80` | `0x14114` | Enable bit0, clock-on bit8, busy bit16 |
| HUBP_CLK_CNTL | `0x5f4 / 0xdc` | `0xead0` | HUBP clock enable and observed clock state |
| DCHUBP_MALL_CONFIG | `0x5f6 / 0xdc` | `0xead8` | MALL selection bits1:0, cursor selection bit2 |
| DCHUBP_MALL_SUB_VP | `0x5f7 / 0xdc` | `0xeadc` | Subviewport start-line mode and timing |
| DCN_VM_AGP_BASE | `0x47a` | `0xe4e8` | Completes the already captured AGP window |
| DCN_VM_CONTEXT0_CNTL | `0x559` | `0xe864` | Depth bits2:1 and block size bits6:3 |
| Context0 base/start/end HI,LO | `0x55a..0x55f` | `0xe868..0xe87c` | Inherited page-directory entry and logical page interval |

The six per-pipe entries occupy indexes `pipe*6..pipe*6+5`, and the eight VM
words occupy indexes24..31 in the listed order. The capture reads timing first,
then guarded HUBP ancillary state, then VM words. A rejected VM word therefore
does not hide the earlier topology readings. A literal all-ones value always
aborts under this conservative read policy; it remains visible as a successful
raw callback with validity clear. This is not a conclusion about whether an
all-ones page-table word has a legitimate hardware encoding.

There are two fixed passes and at most208 reads. Each pass checks all four
HUBP power triplets first, reads timing, guards each HUBP ancillary read and
checks power after each pipe, reads VM words, then checks all power triplets
again. A current PGFSM0 status (`0` or `0x10000000`) and CONFIG0/1 are required;
OFF/transition/unknown values stop before the gated window. Raw power values
must remain identical within and across passes. No register write, indirect
index access, reset, wakeup, looped polling or window probing occurs.

Each pass has independent attempted, callback-success and valid masks. Only a
successful callback stores a value; all-ones or a lost permission check retain
the raw value but leave validity clear. A callback failure leaves that word
zero and invalid, even if the callback assigned an output before failing.
Transport failure, all-ones, revocation or power trouble stops globally and
retains all partial evidence and the last power probe. `changedMask` compares
only words valid in both passes; every bit difference counts, including live
status bits. If all reads complete but values differ, the result is `Changed`
with `readsComplete=true`. A changed power guard aborts with incomplete reads.
No mask or status represents a permission to adopt a mode.

## Source-backed questions

Linux sources are pinned to
`3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`, and the local checker binds their
hashes plus all40 resolved data/power offsets and relevant field masks.

- DCN3.2 uses `dcn10_init_pipes`, which checks each OTG's enable state before
  shutting pipes down. `optc1_is_tg_enabled` reads OTG_MASTER_EN.
  [DCN3.2 dispatch](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/hwss/dcn32/dcn32_init.c),
  [inherited pipe initialization](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/hwss/dcn10/dcn10_hwseq.c),
  [OTG read helper](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/optc/dcn10/dcn10_optc.c)
- `optc32_disable_crtc` clears OTG_MASTER_EN and VTG enable, then waits for
  OTG_BUSY=0. Seeing those fields distinguishes actually stopped timing from a
  momentarily blank HUBP. It does not prove every requester or firmware source
  has stopped, so this change does not classify or skip such pipes.
  [DCN3.2 stop sequence](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/optc/dcn32/dcn32_optc.c)
- `hubp3_read_reg_state` reads MALL configuration and subviewport state, and
  DCN3.2's function table uses it. Extra HUBP reads use the same exact power
  guard as the existing snapshot. MALL/SubVP observations help identify modes
  where a simplistic inactive-pipe interpretation would be inadequate.
  [HUBP reads](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/hubp/dcn30/dcn30_hubp.c)
- DCN3.2 uses `hubbub3_init_dchub_sys_ctx`, which programs FB/AGP and configures
  VMID0 when a page interval is present. `dcn20_vmid_setup` writes depth/block
  size, interval and directory entry, then reads directory-entry LO bit0 as an
  update-ready indication. There is no context-enable field in the captured
  CNTL register. These values cannot establish the meaning of firmware TLB0 or
  replace source evidence for its physical-address conversion.
  [DCN3 hubbub](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/hubbub/dcn30/dcn30_hubbub.c),
  [VM context programming](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/dcn20/dcn20_vmid.c)

MPC connectivity is a possible later extension, not part of this minimum set.
Its TOP/BOT/OPP/MUX state would show routing, but disconnection alone is not a
proof that a HUBP cannot request memory. The existing backend observer already
defines those registers for a separately guarded, complete route inventory.

Run `scripts/check-boot-display-context-local.py --output <fresh-directory>
--discovery .research/rom/discovery-00195d00.bin` for strict C++11, UBSan and real
macOS kernel-header checks. Callback tests exercise every failure/all-ones read,
every pre/post native-permission check, every valid raw-value change and power
guard transition. They are protocol tests; no target GPU execution is claimed.
