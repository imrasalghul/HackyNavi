# Next inherited HDMI stream owner

This proposal follows the reviewed native observation integration. It adds to
[the terminal shutdown design](inherited-hdmi-terminal-shutdown.md), without
reassigning the OPP or MPC/OTG implementations. No production code, hardware
state, startup order or native build was changed by this audit.

The next contained implementation is a **separate inherited-stream transaction**
with private capture, exact expected register changes and a sticky mutation
journal. The existing cold `hdmistream::Controller`, `dio::Link` and
`hdmitmds::deactivate` cannot supply that history. Their saved clocks, packet
ownership, configured link phase and sink capabilities were established by
their own preceding prepare/enable calls. Copying a diagnostic capture into
those controllers would manufacture ownership.

## Exact source decisions

All Linux links use commit `3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`.
`build/boot-hdmi-prerequisites-audit/source-review.json` binds the inspected local
files, the newly retrieved primary sources, and this proposal.

| Decision | Actual DCN3.2 route and implication |
|---|---|
| AVMUTE | `dcn32_init.c` uses `dcn30_set_avmute`; that calls the TMDS encoder's `enc1_stream_encoder_set_avmute`, then observes VACTIVE and three VBLANK→VACTIVE cycles while TG is enabled. A successful HDMI_GC readback or a guessed millisecond delay does not prove those cycles. See [dcn30_hwseq.c](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/hwss/dcn30/dcn30_hwseq.c#L842). |
| Audio identity | `dcn32_stream_encoder_create` maps AFMT and VPG instances to the selected **FE/engine**, not to the PHY/BE or TG index. The observed independent route must be preserved. See [dcn32_resource.c](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/resource/dcn32/dcn32_resource.c#L1271). |
| Audio mute | The installed `enc3_audio_mute_control` dispatches to actual `afmt3_audio_mute_control`, which clears AFMT_AUDIO_SAMPLE_SEND. `afmt3_construct` supplies no poweron/powerdown callbacks. Do not add the generic optional SRAM-power action. See [dcn30_afmt.c](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/dcn30/dcn30_afmt.c). |
| Audio clock | `disable_dio_audio_packet` always mutes packets, but calls HDMI audio disable only for an actual audio resource. `enc1_se_hdmi_audio_disable` then clears AFMT_AUDIO_CLOCK_EN. Its helper explicitly comments out CLOCK_ON polling as unreliable; the port may verify the programmed bit but must not label that as a sourced physical clock-off certificate. Native sibling-HDA DMA quiescence does not identify the AFMT endpoint or prove these packets stopped. See [link_hwss_dio.c](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/link/hwss/link_hwss_dio.c#L285) and [dcn10_stream_encoder.c](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/dcn10/dcn10_stream_encoder.c#L1367). |
| Generic packets | DCN3.2 installs `enc3_stream_encoder_stop_hdmi_info_packets`, covering generic slots **0–14**, with controls 0/6 and line controls 1–4/7–10. The last line-control operation is RMW. A transaction that clears only AVI slot0 does not stop an inherited stream. See [dcn30_dio_stream_encoder.c](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/dcn30/dcn30_dio_stream_encoder.c#L215). |
| Stream reset | `dce110_disable_stream` stops packets and applies `enc1_reset_hdmi_stream_attribute` before resetting input mode and disconnecting FE→BE. The attribute operation changes only packet-generation version, keepout, deep-color enable, scramble and clock ratio; it is not a zero write of the whole HDMI register. See [dce110_hwseq.c](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/hwss/dce110/dce110_hwseq.c#L1200). |
| Optional callbacks | `dcn32_str_enc_funcs` has no `disable_fifo` or `enable_stream`. `dccg32_funcs` has no generic `disable_symclk_se`. Those conditional generic calls therefore supply no DCN3.2 write step. FIFO_ENABLE can remain set through the source-defined terminal path. The MPC/OTG owner already preserves that fact. |
| Sink before stream/link | `link_set_dpms_off` handles sink AVMUTE quirks, PSP stream state and SCDC before stream disable, followed by transmitter disable for HDMI. `write_scdc_data` checks SCDC/quirk information, clears FRL link/update fields, reads sink version, conditionally publishes source version and writes legacy TMDS_CONFIG. A sink without SCDC and a NACKing SCDC sink are different outcomes. See [link_dpms.c](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/link/link_dpms.c#L2392) and [link_ddc.c](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/link/protocols/link_ddc.c#L571). |
| Transmitter | Existing `dmcub::Inbox::transmitter` constructs the actual v1.7 disable command with zero lanes/clock and requires real RPTR consumption. A new inherited caller must separately poll actual DIG_ENABLE request/current state to zero; mailbox consumption alone is not physical completion. `command_table2.c` selects actual DMUB or ATOM execution and may also perform board PHY interlocks. The current native AtomController is an ASIC_Init reset owner, not a transmitter API. |

## Smallest implementation split

Proposed new files are `boot_hdmi_stream.hpp/.cpp`, its protocol tests, and then
a separate native `HackyNaviInheritedHdmi` owner after review. These names are a
proposal, not currently callable APIs.

1. Capture under the actual native exclusion **before any MPC disconnect**.
   Derive FE/AFMT/VPG/TG/BE addresses from real DMU discovery and route from the
   actual ROM. Capture the extra packet/AFMT/HDMI registers in a bounded,
   caller-owned heap workspace, with complete raw baselines, per-bank read
   permission, monotonic total deadline and fail-closed validation. The current
   117-register capture does not include the complete audio or packet state.
2. Assert AVMUTE and observe the source's three frame transitions, then mute the
   actual AFMT packet source. Mark each potentially delivered write before
   issuing it. Readback records programmed state separately from frame progress.
   A sink's skip-AVMUTE quirk must come from actual acquired sink information;
   the first profile can reject that quirk rather than inventing behavior.
3. Leave a private `MutedRetained` state for composition with actual OPP blank,
   HUBP drain and MPC detach. Keep TG, PHY and clocks live. After detach, do not
   rerun the active-route classifier: it correctly rejects a disconnected MPC.
   Instead validate retained stream/timing bank baselines and the precise
   disconnected state supplied by the actual same-device MPC owner.
4. Only after the separately owned PSP/HDCP and sink/SCDC prerequisites, stop all
   generic packets, reset the sourced HDMI fields and input mode, then disconnect
   FE from BE. This establishes stream-programming history only. It does not
   declare the transmitter off or authorize timing/clock release.
5. A native completion method issues the genuine transmitter command through
   the same Display's ready inbox, observes physical DIG request/current zero,
   and publishes a **private same-owner completion** to MPC/OTG `stopTiming`.
   Transport completion, physical state and sink outcome remain separate
   journal fields. Any ambiguous write, timeout, revocation, route change or
   mailbox failure seals the owner and retains all relevant resources.

For the first portable slice, implementation can stop at the private muted
state while the remaining real owner is built. This is a concrete partial
transaction, not permission for native startup to execute it. Tests should
cover exact instance independence, all packet registers, three distinct frame
cycles, skipped/unchanged frame timeout, every meaningful write ambiguity,
changed raw baseline, all-ones/access loss, and no retry after mutation. They
are protocol tests; actual target behavior remains unmeasured.

## Native admission gaps that must be closed

The current observation runs after inherited DMCUB stop, before the driver's new
DMCUB has booted. It cannot send a transmitter command there. A later actual
`DisplayController::Ready` supplies an owned firmware image, mailbox and inbox;
the terminal owner must re-capture and use that real controller without
reconstructing `dio::Link` state. It needs the original normal FunctionGroup
operation and paired-audio provenance, the actual Display→Firmware→GART lock
order, fresh D0/mapping access before and after reads, one outer deadline and a
single retained generation. No cold-clock guard should be weakened to admit it.

Before writes, the native owner still needs concrete proofs for:

- AFMT endpoint/audio state and HDA ownership, rather than a copied Linux
  `audio->enabled` boolean. Already-quiesced HDA does not prove packet state.
- Actual HPD/sink EDID and known sink quirks, routed DDC/GPIO arbitration and
  crystal-derived clock, plus SCDC read/write outcomes. Do not infer SCDC
  support from source scramble bits alone.
- Board route and external encoder/retimer/interlock constraints. The current
  v1.5 route parser rejects USB-C and validates DDC/HPD/PHY, but is not a full
  absence proof for every external display path or ACPI transition policy.
- Actual PSP/HDCP ownership or a source-backed observed condition showing that
  no such actor can rewrite the stream. The Linux callback's absence in a
  newly constructed software object is not an inherited-hardware proof.

These gaps prevent declaring the complete terminal owner callable today. They
do not invalidate the completed read-only observation or independent offscreen
render path. Once the stream/transmitter owner is real, it composes with the
root-owned OPP and separately reviewed MPC/OTG state; physical timing stop still
does not grant clock, power-domain, GART or DMA allocation release authority.
The first profile remains HDMI-TMDS, not FRL or DP/DSC at 4K165.
