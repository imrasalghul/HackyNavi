# Inherited HDMI-TMDS terminal shutdown

This is a source-bound plan and a disconnected MPC/OTG subtransaction, not a
complete display shutdown owner. No target MMIO, firmware command, GPU work,
installation or remote test was performed for this slice.

All Linux references below use commit
`3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`. The local validation report hashes
the exact files and generated DCN3.2 register definitions. Additional unmodified
files are recorded in `.research/inherited-hdmi/fetch.json` with their exact raw
GitHub URLs, byte counts and SHA256 values.

## Actual ordering and the remaining integration

The source has two related sequences, not one complete transferable software
state object:

1. `dcn20_program_front_end_for_ctx` blanks pixel data using the OPP DPG before
   disconnecting an MPCC. `dcn20_post_unlock_reset_opp` waits for double-buffered
   disconnection before resetting OPP resources. `dcn10_wait_for_mpcc_disconnect`
   waits for MPC idle while the real TG is enabled, then blanks the HUBP.
2. `link_set_dpms_off` waits for link exclusion; requests AVMUTE unless the sink
   quirk forbids it; disables audio; informs the actual PSP stream owner;
   invokes `blank_stream`; restores sink SCDC to legacy TMDS; stops the HDMI
   stream; and disables the link output. For HDMI, stream disable precedes
   transmitter disable. `dce110_blank_stream` does **nothing for HDMI** and is
   not a HUBP blank/drain operation.
3. `dce110_disable_stream` stops all generic HDMI info packets and resets the
   HDMI stream attributes, disables audio again, then calls the DIO stream
   reset. That reset sets input mode to zero and disconnects DIG FE from BE.
   Its FIFO-disable and stream-disable calls are optional. The concrete
   `dcn32_str_enc_funcs` provides neither optional callback, so the generic
   function does not establish a DCN3.2 FIFO-disable write. Likewise the
   concrete `dccg32_funcs` does not expose the generic `disable_symclk_se`
   callback: its presence in a generic branch is not an executed DCN3.2 step.
4. `dcn10_link_encoder_disable_output`, used by the DCN3.2 link table, executes
   the real VBIOS transmitter-disable request. It skips an already disabled
   DIG and does not itself invent a successful PHY state. A successful firmware
   transport still needs physical DIG-enable/current-state readback.
5. `dcn20_reset_back_end_for_pipe` disables the actual root TG after link
   teardown, then disables its clock. `optc32_disable_crtc` disconnects all
   four OPTC inputs, clears OPTC memory selection, clears OTG master enable,
   clears VTG enable, and waits for OTG_BUSY=0. Clock gating is a **separate**
   operation. Later software symbol-clock reference updates can cause another
   output-disable operation. An inherited owner cannot manufacture those
   reference counts by constructing a zeroed Linux-shaped object.

A future native transaction must own the same device/reset/power generation,
fresh PCI D0 and memory decoding, immutable ROM/discovery, all display clients,
the actual DMCUB/PSP/HDCP programmers, DDC pins/engine and audio users. It must
retain every attempted mutation and all resources on any ambiguous result.
It cannot infer these exclusions from two matching diagnostic snapshots.

The planned composition is: acquire exclusions and capture → OPP replacement
and completion → actual HUBP blank/drain → MPC detach with TG live → real HDMI
audio/packet/sink/firmware teardown → timing stop → separately proved clock and
power retirement. Blanking all captured HUBPs before MPC detach is an explicit
terminal port policy; it does not change Linux's double-buffer requirement that
the TG remain alive until MPC idle. Every stage rechecks the actual retained
hardware epoch. A failure never advances the next stage or retries the previous
mutation. Missing ownership prevents entry; it is not converted into success.

## Existing implementations and exact gaps

| Substep | Reusable implementation | Required inherited ownership or evidence |
|---|---|---|
| Route observation | `bootbackend::capture`, real native read-only bridge | Its 117-register observation grants no write, PHY, sink, clock or DMA authority. |
| OPP replacement | Separate `boot_opp_blank` owner under development | Actual color/dimensions, selected OPP, DPG enable and pending completion. EN alone does not prove black pixels. |
| HUBP readers | Actual private `bootdisplaystop::State` after `stop` | Keep the same native owner/epoch and freshly compare every powered reader; OFF windows must remain untouched. |
| MPC/OTG | New `bootbackendstop::Controller` described below | No native callsite yet; all exclusions and later HDMI teardown remain mandatory. |
| AVMUTE / all HDMI generic packets | Source-defined register sequence in `enc1_stream_encoder_set_avmute` / `enc3_stream_encoder_stop_hdmi_info_packets` | The inherited VPG/packet/AFMT inventory is not in the 117-register capture. Cold `hdmistream::Controller` cannot be adopted; its saved clock/buffer state belongs to its own enable sequence. |
| Audio | Actual AFMT sample-send mute; optional endpoint/audio-clock handling | The HDA-function `audio_quiesce` DMA proof is a different domain and does not establish AFMT endpoint identity or packet shutdown. DCN3.2 uses `afmt3_construct`; that concrete table lacks AFMT powerdown. |
| SCDC | Real `hdmiddc::Engine` transfers and readback | Acquire actual GPIO/I2C arbitration, derived clock, HPD/sink capabilities and sink quirks. SCDC support cannot be inferred solely from source scrambling bits. The upstream helper reads sink version, conditionally writes source version, clears FRL fields, then writes TMDS_CONFIG=0 for legacy. Do not send a guessed ACK. |
| Transmitter | Actual `dmcub::Inbox::transmitter` or bounded ATOM interpreter | Inbox must be the real ready owner of this ROM and route. Native AtomController currently exposes post-reset ASIC_Init, not inherited transmitter execution. Do not substitute that path or a success callback. Preserve actual firmware/physical readback separately. |
| External devices / HDCP | No inherited owner established | ROM route parsing rejects the v1.5 USB-C field; that is not proof that every retimer/redriver, external encoder, sink quirk or HDCP actor is absent. |
| Symbol/PHY/TG clock release | Existing cold clock controllers have their own journals | Derive actual inherited references and terminal order; do not reset software reference counts and declare clocks unowned. |

## Implemented MPC/OTG subtransaction

New files: `include/hackynavi/boot_backend_stop.hpp`,
`src/boot_backend_stop.cpp`, `tests/boot_backend_stop_tests.cpp`.
The caller heap-owns a noncopyable controller and keeps the actual stopped HUBP
State alive. `capture` performs its **own** real backend capture; no caller-made
capture/valid flag is accepted. It derives addresses from the exact DMU record,
privately retains the observations, and reads selected VTG/OPP DPG controls.

It does not infer HUBP n → DPP n from matching indices. Instead it requires the
actual whole four-HUBP stop state and rechecks all power/status/config values,
every powered pipe's full captured programming (including cursor and DMDATA),
blank/no-outstanding status and clocks. OFF windows are skipped only while the
original exact raw OFF status and config persist. This checks current hardware
against private software history; the native owner must separately guarantee
same-device provenance and exclusion for the lifetime.

`disconnect` preserves TG master/current state, VTG enable and the captured
clock fields. It updates only the selected output mux low nibble to 0xf,
preserving the remaining output fields (`REG_UPDATE` upstream). It then writes
TOP, BOTTOM, OPP and UPDATE_LOCK selectors to 0xf (`REG_SET` upstream), in that
order. Immediate programmed-value readback is necessary but is **not** accepted
as active disconnection: bounded polling also requires MPCC_IDLE=1 and BUSY=0.
The MPC muxes are double buffered; TG remains live through the completion check.
Both idle encodings 1 and 5 are accepted after detach; contradictory/unknown
status encodings are rejected. Other routes stay unchanged.

`stopTiming` is a separate call. It requires physical selected DIG-enable=0,
FE selection empty, unchanged FIFO programming, DP-stream=0, AVMUTE=1 and the
source-backed HDMI attribute reset. The concrete DCN3.2 table has no FIFO-disable
callback: FIFO_ENABLE may remain set, and no write is invented to clear it.
Read-only DIG_SYMCLK_FE_ON may be zero or one after the actual outer transmitter
operation; programmed FE source/mode bits remain exact. The selected OTG/VTG
clock observations still require their separately retained states.
These are supplemental physical checks, **not** a token
representing outer audio/sink/firmware completion. No native API calls this
method until that real owner exists. It then disconnects OPTC segments, clears
memory selection, clears OTG master and VTG, and waits for both actual current
master state and OTG_BUSY to clear. It changes no clock control, DPG, transmitter,
HUBP, memory allocation or firmware mailbox.

The source-backed selected extra registers are:

| Register | DMU DWORD offset | Field checked |
|---|---:|---|
| VTGn_CONTROL | BASE_IDX2 + 0x052e + n | ENABLE bit31; all other bits retained, reserved bit15 rejected |
| DPGn_DPG_CONTROL | BASE_IDX2 + 0x1854 + n×0x5a | EN bit0, exact supported raw control retained |
| DPGn_DPG_STATUS | BASE_IDX2 + 0x185b + n×0x5a | DOUBLE_BUFFER_PENDING bit0 must be zero |

`opp2_dpg_is_blanked` at lines359–374 uses EN=1 and pending=0;
`dcn20_wait_for_blank_complete` at2586–2607 first waits for pending then checks
the blank predicate. This subtransaction neither assumes nor proves the chosen
DPG color. Its MPC order comes from `mpc1_remove_mpcc` at269–331; completion from
`mpc1_assert_idle_mpcc` at109–119 and `dcn10_wait_for_mpcc_disconnect` at3616–3643.
OTG write ordering comes from `optc32_disable_crtc` at167–199.

Each of nine possible writes sets its private attempted bit **before** the
callback and its acknowledged bit only after guarded actual readback. Permission
and lexical deadline are checked before and after accesses. Polling has a
bounded per-call delay budget, while the native `valid` callback must enforce
the one outer elapsed-time deadline including register guards. All accepted
hardware-phase errors seal Faulted; there is no retry or reset-clear method.
TimingStopped retains the owner because clocks and outer resources remain.

## Validation scope

`scripts/check-boot-backend-stop-local.py` performs strict and UBSan protocol
checks, compares the addresses and key field masks to the exact pinned generated
table, and compiles the real source against MacKernelSDK with kernel flags and a
2KiB frame limit. Tests cover actual API-created stopped reader state, MPC/TG
write order, preserved unrelated mux fields/clocks, per-write ambiguity, dropped
writes, timeout, stale power/reader/DPG/clock state, OFF-domain avoidance and read
failures at distinct registers and mutation boundaries. They are offline numeric
register callback tests, not successful target shutdowns. Earlier exhaustive
failure repetition was reduced to distinct semantic boundaries for final runs.
