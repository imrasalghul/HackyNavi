# Inherited HDMI stream mute transaction

`boot_hdmi_stream.hpp/.cpp` implements the contained source-side AVMUTE/audio
phase of the [inherited HDMI owner design](boot-hdmi-stream-owner-prerequisites.md).
Its endpoint is `MutedRetained`, not stream shutdown or display retirement.
It has no native callsite. There has been no target MMIO, sink transaction,
firmware command or GPU execution for this component.

## Actual sequence and retained evidence

The noncopyable, heap-owned controller derives DMU 3.2 register addresses from
the actual discovery table and privately runs the existing checksummed-ROM,
two-pass 117-register HDMI route capture. The selected FE/engine determines
the AFMT/VPG instance independently of the PHY, TG, OPP, MPCC and DPP indices.
No caller-populated route or success flag is accepted.

It inventories 30 additional selected registers: AFMT clock, source, channel,
packet, status and memory controls; HDMI metadata, audio, ACR, audio infoframe,
all 15 generic send/continuous controls and line scheduling, immediate requests,
double-buffer state and active AVMUTE; and VPG update/status/memory controls.
It brackets this inventory with the already-enabled AFMT clock and a second
complete matching raw observation. No auto-increment SRAM data is read.

The first accepted profile is deliberately bounded. It requires the already-on
AFMT clock, immediate HDMI double-buffer mode, no AFMT sample double buffering,
no update requests/pending flags, no VPG conflict/lock, awake unforced SRAM,
ordinary audio channels, no HBR/DP/test/override modes, no metadata/MPEG/ISRC/ACP
producer, and no error/strobe/acknowledgement fields that an RMW could replay.
Unknown or changing encodings are rejected before either write. A read-only
audio source number is retained as evidence; it does not establish HDA endpoint
ownership. Normal audio/ACR and all generic packet scheduling remain unchanged.

`mute()` executes exactly two source-defined RMW operations under the same
native exclusion/deadline callback:

1. Set `HDMI_GC.AVMUTE`, preserving its accepted raw baseline. Record possible
   delivery before the write; acknowledge only exact register readback.
2. Observe an actual `OTG_STATUS.V_ACTIVE_DISP`, followed by three separately
   observed `V_BLANK` then `V_ACTIVE_DISP` pairs. The selected TG request/current
   state, clock, entire route and packet/audio baselines are checked around
   each observation. Contradictory active+blank or unsupported status bits fail.
   An already-set AVMUTE bit does not skip the frame observations. Require
   actual `HDMI_STATUS.ACTIVE_AVMUTE` after the three cycles.
3. Clear `AFMT_AUDIO_PACKET_CONTROL.AUDIO_SAMPLE_SEND`, preserving its accepted
   immediate-mode configuration. Record the second attempted/acknowledged
   write independently and recheck all retained state before `MutedRetained`.

There is one shared caller-supplied iteration/delay budget across all seven
frame waits, capped at 700 ms of requested delay. This reflects the source's
seven waits of at most 100 ms each; it is not an elapsed-time certificate.
The real native `valid` callback must additionally enforce one monotonic total
deadline including every MMIO access, not merely accumulated sleeps.

Any hardware failure after admission seals the controller in `Faulted`, with
attempted and acknowledged writes and completed cycle count retained. A failed
write may have reached hardware; no rollback, retry or reset-clearing API is
provided. Success retains the object and confers no clock, power, DMA or memory
release authority. Access callbacks must bind the real device/generation and
fresh PCI D0/control-bank permissions before and after every access.

## Exact primary source bounds

All references use Linux commit `3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`.
The local report binds their unchanged files and generated register tables.

- [dcn32_resource.c](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/resource/dcn32/dcn32_resource.c#L1271)
  constructs AFMT and VPG with the selected DIO engine index. Its resource table
  defines five audio endpoints; no endpoint object is reconstructed here.
- [dcn30_hwseq.c](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/hwss/dcn30/dcn30_hwseq.c#L842)
  supplies the actual AVMUTE, initial active and three blank/active sequence.
  The concrete DCN3.2 table selects `optc1_wait_for_state`; its
  [OTG status predicates](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/optc/dcn10/dcn10_optc.c#L827)
  are used directly, without inferred scan-position arithmetic.
- [dcn10_stream_encoder.c](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/dcn10/dcn10_stream_encoder.c#L1033)
  defines the AVMUTE RMW; the DCN3.2 stream table uses it and the concrete
  `enc3_audio_mute_control` forwarding path to
  [dcn30_afmt.c](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/dcn30/dcn30_afmt.c#L146).
  The actual AFMT function table has no poweron/powerdown callbacks.
- [dcn30_dio_stream_encoder.c](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/dio/dcn30/dcn30_dio_stream_encoder.c#L194)
  sets `HDMI_DB_DISABLE` for this update profile and defines the complete
  15-slot generic packet stop sequence. The present component captures that
  state but does not run the later stop operation out of order.

## Why this is not yet a callable complete stop owner

The sampled display registers do not contain a legacy-HDMI HDCP session-status
proof. The actual Linux PSP path queries/destroys HDCP sessions through an
initialized HDCP TA and owned session handle, and removes DTM topology through
its actual initialized TA. A newly allocated empty software object, a copied
boolean or an unprotected-looking pixel format does not establish those facts.
The exact files and URLs are retained in `.research/inherited-hdmi/hdcp-fetch.json`.

Consequently native mutation admission remains disconnected until a genuine
owner acquires protection exclusion, actual sink information including the
skip-AVMUTE quirk, audio ownership, routed DDC/SCDC and board/firmware constraints.
This component neither accepts a protection token nor declares HDCP inactive.

After this phase, the actual outer owner can compose OPP blank, HUBP drain and
MPC detach while retaining this stream state. A later component must perform
the source-ordered PSP/SCDC prerequisites, stop all generic packets, reset HDMI
attributes/input mode and disconnect FE from BE, then issue the real transmitter
disable and observe physical completion. No such later authority is supplied
by `MutedRetained`. In particular, FIFO_ENABLE may remain set on DCN3.2, and
audio-clock programmed readback is not a reliable physical clock-off proof.

## Focused validation

`scripts/check-boot-hdmi-stream-local.py` runs a strict and undefined-behavior
sanitizer protocol suite, checks the selected addresses/masks against the
actual AMD-generated tables, and builds a freestanding x86_64 macOS 15 object
against the actual MacKernelSDK with a 2 KiB stack-frame guard. It binds source
and transitive header hashes before/after the compile.

The offline tests exercise independent instance selection, unchanged 15-slot
packet state, real-status transition predicates, missing/repeated/contradictory
frames, actual-active-AVMUTE rejection, both potentially delivered writes,
permission loss, final audio-send reassertion, raw inventory changes and
terminal no-retry behavior. Their numeric callback model is a protocol test,
not evidence of target hardware behavior or inherited native ownership.
