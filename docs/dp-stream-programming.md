# DCN3.2 DisplayPort stream programming

`dpstream::Controller` implements the DIO video stream stage after a direct
8b/10b SST link has completed training. This is a portable MMIO implementation;
it has not been run on the GPU and is not yet a native modeset entry point.
It is independent of the first offscreen graphics test that preserves HDMI.

The supported stream is progressive RGB, 8 bits per component, full range,
one pixel per cycle, one OPP and OTG, and no secondary packets. The exact trained
link accepts 1/2/4 lanes and 162/270/540/810 MHz symbol clocks. Admission checks
the necessary RGB24 payload bound against that actual rate/lane pair, not the
sink maximum. This does not establish DML bandwidth, sink picture, DSC, UHBR,
MST, audio, HDR, or 4K165 support.

## Source sequence

The implementation follows Linux commit
`3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`:

- [DCN1 MSA attributes, stereo routing and vblank stop](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/dio/dcn10/dcn10_stream_encoder.c).
- [DCN2 SST SDP splitting control](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/dio/dcn20/dcn20_stream_encoder.c).
- [DCN3.2 M/N, steer and DIG FIFO start sequence](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/dio/dcn32/dcn32_dio_stream_encoder.c).

Preparation validates the discovered DCN3.2 layout and the physical DIO route,
trained lane count, completed training state, video-pattern framing/scrambling,
and cold FIFO/stream. It rejects foreign FE consumers and active/pending
secondary packets, metadata, DSC, FEC, bypass and VBID overrides. It programs
FE/OTG stereo-gated routing, RGB8 format, MISC0/MISC1, the four exact MSA timing
registers, and single-pixel FIFO/SDP settings while video remains disabled.

Start validates both the saved stream programming and the running OTG's totals,
blanking, sync positions/polarities, DP start point, clock and progressive state.
It disables M/N generation, seeds N=32768 and
M=floor(N*pixelClockKHz/symbolClockKHz), then enables auto measurement with no
N multiplier. It observes stream-off before resetting steer, waits 10 us,
releases steer, waits FE symbol clock, programs the FIFO read level to 7,
asserts/deasserts FIFO reset with both reset-done observations, enables FIFO,
waits 100 us, and enables video. Hardware performs the enable at the next frame.
Success additionally requires enabled stream status, no FIFO/steer errors,
two forward OTG frame transitions, and final route/timing/configuration checks.
Auto-measured M is not incorrectly compared with its initial seed.

Stop selects disable at the next vertical blank, clears enable, and waits
until both stream enable and stream status are zero **before** asserting steer
reset. It then disables DIG FIFO and M/N generation and verifies stream-off.
It does not manufacture VBID field overrides. OTG and PHY must remain running
until this sequence finishes; stream-stop alone does not retire scanout DMA.

## Native integration contract

The caller must retain actual trained DIO/PHY, clock/DCCG, OTG, display bandwidth,
and HUBP/DPP/MPC/OPP surface owners, with firmware/PM/reset/modeset actors excluded.
`Io::valid` is mandatory and must validate those owners, the active operation,
the mapping, and the power generation. Every attempted read, write and delay
has a validity check before and after the callback, including failed I/O.
The interface is not a substitute for acquiring those resources.

Configuration, layout and poll budgets are copied before callbacks. Layouts
are independently rebuilt from their discovered base and rechecked against all
register addresses before access. A write is considered touched before calling
the transport, so a failed write cannot release ownership. Any post-write
failure leaves `Faulted` and `requiresHardwareRetirement()==true`; stop can
still be attempted only while actual ownership remains valid. Successful stop
leaves steer reset asserted and allows a new prepare. `clearAfterVerifiedReset`
is bookkeeping for an independently verified reset, never a recovery action.

Shared build integration includes `src/dp_stream.cpp` in the portable/core
source lists and `tests/dp_stream_tests.cpp` linked with discovery. Native adapter
integration remains separate and is not included in these checks.

`build/dp-stream-cmake-integration-1/report.json` verifies the actual CMake
target and its 291,840 assertions. The focused source-oracle run adds two
live-discovery checks for 291,842 strict and UBSan assertions each. These are
offline assertions, not a sink picture or GPU execution result.

## Focused validation

Run `scripts/check-dp-stream-local.py --output <fresh-directory> --discovery
.research/rom/discovery-00195d00.bin` using Python. It verifies pinned upstream
file hashes, all 228 derived register locations/base indices, the relevant
generated field masks, exact numeric timing/M/N and protocol ordering, 100
FE/BE/OTG combinations, failure at every lifecycle I/O, ambiguous writes,
revoked ownership at every validity check, dropped changing writes, input
mutation, malformed layouts and bounded polls. Strict and UBSan test builds
are followed by a real MacKernelSDK header compile of the production source
for x86_64 macOS 15 with a 2 KiB frame limit. Negative-control source copies
must fail when a post-write ownership check or the vblank-before-steer stop
wait is removed. None of these checks execute GPU MMIO.
