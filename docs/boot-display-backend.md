# Inherited HDMI backend observation

`bootbackend::capture` is a read-only prerequisite for a future terminal
inherited-display owner. It parses the actual DMU discovery and checksummed
legacy Navi31 Atom image, reads 117 distinct controls twice, and classifies one
stable HDMI-TMDS RGB route. It does not stop hardware, acquire a route, authorize
clock changes, retire DMA, or reconstruct an existing cold controller.

The capture resolves the independent physical PHY/HPD/DDC values through the
actual ROM GPIO LUT and connector records. It then follows observed DIG backend
selection to a frontend, frontend source to an OTG, OPTC source to an OPP, output
mux to an MPCC, and MPCC top to a DPP. These indices need not be equal. Numeric
tests cover all 1,280 FE/TG/OPP/MPCC/DPP combinations. The supplied ROM resolves
HDMI path 3 to PHY 3, DDC 0 and HPD 0; test register values are constructed numeric
fixtures, not measurements of the installed GPU.

Every register access requires the caller's live `valid` guard and address/domain
`canRead` permission before and after the transport callback. The native seam
must bind these to the real FunctionGroup Operation, MMIO lifetime, clock/power
access constraints, elapsed deadline, and exclusion of competing display,
firmware and PM writers. The helper does not create that exclusion. All encoder
muxes are inspected before legacy frontend registers; any HPO selection or
unknown mux encoding stops the inventory. There are no retries or write,
firmware-command, delay, allocation, or hidden-global callbacks.

Raw observations and per-bank read masks reside in the caller's `Capture`.
The second pass compares directly against the first; no second large stack
snapshot is allocated. `valid` remains false after every failure, including
revocation immediately after a successful read. Successful all-ones transport
is distinguished from a failed read. Partial masks are diagnostic only.

The initial profile rejects multiple or shared routes, DP and HPO/FRL, DSC,
multiple ODM segments, interlace, stereo/bypass/split encodings, update locks,
unsettled enables and unclassified active control encodings. It retains the
actual HDMI scramble, quarter-rate and 8/10/12/16-bit container observations.
It neither computes pixel clock nor claims the sink's SCDC matches those values.
The 4K165 monitor's high-rate FRL or DP/DSC operation is outside this first
profile. Connector type and a 4K monitor mode cannot prove the current wire
protocol. Other observations needed for a terminal transition, including audio,
packet-engine and sink state, are still required before writes.

## Source contract and subsequent owner

The pinned Linux revision is `3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`.
The focused validation binds the exact source files by SHA-256 and checks every
sampled register instance and base index against its generated DCN3.2 table.

- `dcn_3_2_0_offset.h` and `dcn_3_2_0_sh_mask.h` define each address, stride and
  field. `DIO_LINK*_CNTL.ENC_TYPE_SEL` selects legacy/HPO; DIG frontend selection
  is a one-hot field, while its source and OPTC/MPCC selectors are numeric.
- `dcn10_link_encoder.c` maps DIG mode 3 to HDMI and identifies the actual
  transmitter disable operation. Its disable path warns against touching a
  previously inactive PHY and requires the real transmitter/HPD/connector route.
- `dcn32_dio_stream_encoder.c` supplies the HDMI mode/container semantics;
  `dcn32_optc.c` supplies the real OPTC segment and disable sequence.
- `dcn10_mpc.c::mpc1_remove_mpcc` disconnects the output/top/bottom/OPP/update-lock
  muxes. `dcn10_hwseq.c::dcn10_wait_for_mpcc_disconnect` waits for MPCC idle while
  its timing generator is enabled. A blanked HUBP is insufficient evidence.
- `link_dpms.c` distinguishes HDMI stream-before-link shutdown from DP1 SST's
  link-before-stream sequence. HDMI includes AVMUTE, audio/packet shutdown and
  sink SCDC changes. `dcn20_reset_back_end_for_pipe` completes DPMS-off before
  OTG/VTG disable and timing-clock removal. These calls depend on actual owned
  topology and cannot be replaced with zeroed objects or success replies.

The future native owner must retain the private capture and revalidate every
relevant raw field under actual exclusion before the first mutation. Its
one-shot journal starts before any possibly delivered change. A source-backed
HDMI terminal sequence must coordinate inherited HUBP blank/drain, MPCC
disconnect while timing still runs, audio/packets/AVMUTE, real DDC/SCDC state,
firmware transmitter disable with observed DIG disable, FE disconnection, and
OTG/VTG/clock shutdown. Exact cross-block ordering and any HDMI symbol-clock
reference need the full source sequence reviewed before implementation. Failed
or ambiguous writes retain the owner; capture success is never a release token.

## Startup boundary

The current Engine queue-before-DisplayBoot order still permits the offscreen
render profile. `Display::boot` does not require global clock journals. Global
clock acquisition later requires no queue/VM owner and actual physical display
idle; the existing `bootdisplaystop` only drains HUBP and leaves OTG/MPC/DIO.
This new observation does not change that order or relax either exclusion.

Moving Display earlier prematurely would also conflict with Engine's existing
no-queue/Display failure guard and Firmware's strict no-queue RLC retirement
guard. A future coordinated startup transaction must first complete downstream
ownership, source-derived clock/mode selection, and retirement of retained
policies. No startup activation or native capture callsite is added here.
