# G90XF monitor evidence and 4K165 requirements

Read-only capture on 2026-10-10 at 23:47:46 UTC identifies an active Samsung
`Odyssey G90XF`, product `SAM7859`. User-reported connection information:
the monitor works through either connector and is currently connected over HDMI.
This report does not turn that statement into a measured DisplayPort result.

Samsung lists this model as the 27-inch Odyssey 3D G90XF, 3840×2160 at 165 Hz,
with two HDMI 2.1 inputs and one DisplayPort 1.4 input. It is not identified by
this capture as an OLED model. [Samsung model specifications](https://www.samsung.com/us/monitors/gaming/odyssey-3d-g90xf-27-inch-165hz-uhd-ls27fg900xnxza/).

## Captured state

`scripts/capture-windows-display.py` uses only read APIs: QueryDisplayConfig,
DisplayConfigGetDeviceInfo, and WMI raw EDID acquisition. It does not change a
mode, write registers, install anything, or touch OpenCore media. The target
identity is checked against the returned mode array; WMI identity and raw block
results are retained separately. Queries are sequential, not an atomic hardware
snapshot. WMI may supply the Windows driver's cached EDID.

Local evidence is in `build/monitor-evidence/20261010T234746595065Z/`:

- `capture.json`: Windows path, timing, color, WMI identity and raw block data.
- `active-monitor-0.edid`: all 384 bytes, each 128-byte checksum zero.
- EDID SHA-256: `3a34c8ef8a999969b79f6d8af1af8b7bf29b42d70b9642af646c7c5840b88122`.
- Collector SHA-256: `f834c5d29fdbb8fdc9910929e1387ea2d3dfae89355035f92e186b739dc2c6c1`.

Raw identity/serial data remains in ignored build evidence. Tests embed only
the relevant capability and timing bytes, without the identifying EDID base.

| Windows driver report | Value |
| --- | --- |
| Active connector technology | 5, HDMI |
| Active target | 3840×2160, progressive |
| Path / target refresh rational | 165/1 Hz |
| Target pixel clock | 1,629,250,000 Hz |
| Horizontal / vertical totals | 4224 / 2338 |
| Color encoding / component depth | YCbCr 4:4:4 / 10 bits |

Microsoft defines technology 5 as HDMI and 10 as external DisplayPort. Signal
fields are the Windows driver's target signal description, not direct wire
measurements. [Connector enum](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ne-wingdi-displayconfig_video_output_technology),
[signal structure](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_video_signal_info).
Neither queried interface supplies a negotiated FRL lane rate or active DSC
status. This capture proves neither. Color depth is recorded as evidence; HDR
implementation remains deferred.

The base EDID says one extension. First-CTA HF-EEODB overrides this to two
extensions, so a 256-byte-only read would lose the high-refresh timing.
The second CTA extension has a Type VII revision 2 timing at offset 4:

| EDID timing field | Value |
| --- | --- |
| Pixel clock | 1,629,250 kHz |
| Horizontal active / total | 3840 / 4224 |
| Horizontal sync start / end | 4000 / 4032, positive |
| Vertical active / total | 2160 / 2338 |
| Vertical sync start / end | 2163 / 2173, negative |

The exact clock/totals imply approximately 164.975447 Hz; Windows reports the
nominal 165/1. No porch, polarity or clock was inferred from the product name.

## Sink advertisements

The first CTA's HDMI Forum VSDB is at offset 42 with payload length 13. Raw
bytes including its CTA header are `6d d8 5d c4 01 78 80 6b 00 30 a5 c3 64 3f`.

| Capability | Advertised limit |
| --- | --- |
| TMDS character clock | 600 MHz |
| SCDC | Present |
| Maximum FRL code | 6: four lanes at 12 Gbit/s each |
| HDMI DSC | Version 1.2 supported, native 4:2:0 supported |
| DSC higher component depths | 10 and 12 bpc; 16 bpc bit clear |
| Arbitrary 1/16-bit DSC bpp | Not advertised |
| DSC FRL maximum code | 6 |
| DSC slice code | 4: up to eight slices, 340 MHz / KSliceAdjust per slice |
| DSC total-chunk code | 63: up to 65,536 bytes per line of chunks |

These are maxima, not a selected transport. DSC bpc bits are separate from
TMDS deep-color bits. The slice clock still requires HDMI KSliceAdjust and
source-encoder limits; the raw 340 MHz numerator is not standalone admission.
The chunk field uses 1024×(code+1) for nonzero codes; code zero remains
unsupported. References: [pinned Linux EDID parser](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/drm_edid.c#L6141),
[pinned edid-decode HDMI parser](https://github.com/gjasny/v4l-utils/blob/95ad25f6a77a0a6650f5f657ac2c5046efcd04a0/utils/edid-decode/parse-cta-block.cpp),
and [libdisplay-info field documentation](https://emersion.pages.freedesktop.org/libdisplay-info/libdisplay-info/cta.h.html#di_cta_hdmi_dsc).

One reference disagreement is deliberately preserved: DSC slice code 7 means
16 slices / 400 MHz in pinned Linux and libdisplay-info, but the current
edid-decode table says 12 / 600 MHz. The parser retains code 7, leaves its
decoded throughput unavailable, and sets `sliceLimitsDecoded=false`. This
monitor uses code 4, so the disagreement does not affect its decoded limits.

## Implemented parser behavior and remaining driver work

`edid::Monitor` now preserves FRL lane/rate maxima and HDMI DSC fields with
presence and decoded-limit indicators. Duplicate HF-VSDB/HF-SCDB declarations
must agree on the entire SCDS, including DSC data. Advertised DSC with missing
limit bytes, reserved FRL/slice values, and reserved chunk bits are rejected.
Unknown capability fields never authorize hardware operations.

CTA Type VII revision 2 now supplies the real 4K165 timing with bounded integer
decoding and checked geometry sums. Bit 7 is a 4:2:0-only marker in this revision,
not a preferred flag. Interlaced, stereo and 4:2:0-only entries are skipped.
Unknown revisions and Type VIII/X timing blocks remain unsupported. Timing
source bits survive deduplication. [Type VII reference](https://github.com/gjasny/v4l-utils/blob/95ad25f6a77a0a6650f5f657ac2c5046efcd04a0/utils/edid-decode/parse-displayid-block.cpp).

ASan/UBSan host tests passed 2,722 checks. The parser also compiled as an
x86_64 macOS kernel object against real MacKernelSDK headers. The captured EDID
parses successfully into seven supported modes, including the exact 4K165
entry. Reports: `build/edid-monitor-tests-report.json`,
`build/edid-monitor-tests.log`, `build/edid-monitor-actual.log`.

The initial TMDS/RGB24 DML profile cannot drive this 1,629.25 MHz timing.
Remaining work includes HDMI FRL source/pipeline setup and SCDC training,
appropriate DCN clock/ODM/DSC policy, source DSC configuration/PPS and packet
transport where needed, bandwidth calculation for the chosen encoding, and
native lifetime/ownership integration. EDID decoding is not a modeset.

DisplayPort 4K165 is also a final requirement. A DP connection needs its own
live EDID and DPCD capability capture, including receiver DSC/FEC and link
limits; HDMI DSC advertisements cannot substitute for DPCD. DP link training,
DSC encoder/PPS transport and the corresponding DML profile remain necessary.
No macOS high-refresh or accelerated rendering result is established here.

## FRL implementation follow-up

`hdmi_frl.hpp/.cpp` now implements the portable SCDC training sequence for one
caller-selected rate code 1–6. It validates the immutable mode/route identity,
source and sink rate bounds, handles FLT_READY/FLT_UPDATE/lane requests/FFE,
and performs FRL_START separately after entering packet mode. Measured
monotonic deadlines include I/O time: 210 ms for each training stage (the
pinned AMD implementation uses 105 polls at 2 ms), and 200 ms for FRL_START.
Lower-rate requests require caller mode recalculation. Compliance test modes,
unsupported patterns, timeouts and I/O errors never become success. Source
shutdown must complete before resetting the receiver rate; unresolved DDC or
source state retains ownership. [AMD protocol source](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/link/protocols/link_hdmi_frl.c).

`hpo_frl.hpp/.cpp` implements the DCN3.2 singleton HPO FRL link register
sequence: local clock enable with observed clock status, lane/scrambler/LTP
setup, reset/enable, masked lane-pattern changes, packet mode and local
shutdown. All eight register addresses come from generated BASE_IDX3 offsets.
The controller checks awake memory and active-stream conflicts, avoids gated
stream reads and preserves unrelated bits. It retains partial writes and stops
accesses after revocation. It does not implement HPO stream/DSC encoding,
PHY/PLL commands, clocks or native ownership itself. Its callbacks require the
already-held shared operational gate; nesting a second gate acquisition inside
the FRL protocol would be wrong. [HPO link source](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/hpo/dcn30/dcn30_hpo_frl_link_encoder.c).

Both modules passed ASan/UBSan CPU tests (3,021 protocol and 1,397 HPO checks)
and real x86_64 kernel-header compilation. Evidence is in
`build/frl-verification-report.json` and its two referenced test logs. These
tests use simulated receiver/register state to check the real implementation's
protocol and failure behavior; they are not GPU results. No native FRL adapter
is connected yet. In particular, the existing DMCUB DP/TMDS API cannot be used
as an FRL transmitter command: FRL needs explicit signal/engine ABI handling,
actual board route and PHY capability validation, and completion checks.

Next integration dependencies are native FRL DMCUB/PHY support, DCCG character
and stream clocks, HPO stream/tribyte/borrow-mode setup, DSC encoder/PPS where
required, and FRL-capable bandwidth calculation. Public reference paths are
`link_dpms.c:enable_link_hdmi_frl`, `dcn30_hwseq.c:dcn30_setup_hdmi_frl_link`,
`dcn32_dccg.c`, `dcn30_hpo_frl_stream_encoder.c`, and `dml1_frl_cap_chk.c` at the
same pinned revision. Mode-capacity admission must include the FRL packing and
blanking/borrow constraints; multiplying lane rate by lane count is not a full
mode test. DisplayPort DSC/FEC remains an independent final requirement.
