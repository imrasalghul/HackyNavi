# Native timing integration review

Review date: 2026-10-11. This is a source review and dependency proposal, not a
claim of native modesetting or hardware validation. No timing API or hardware
operation was added by this review.

## Required retained owner

Keep the portable `otg::Controller` private to the native mode/surface owner.
`HackyNaviScanoutSurface` already retains the actual allocation, Display lease,
exact powered pipe, HUBP and DPP/MPC/OPP route. Its existing FunctionGroup
Operation and GART Access, followed by Display → Firmware → GART locks, are the
appropriate access boundary. A separate object accepting a pipe, generation,
`GlobalSync` and a readiness boolean would not acquire those dependencies.

The next required binding is a retained native mode calculation record. The
real calculation already exists in `userspace/display/dml32_runtime.c`,
`dml32_plan.*`, `dml32_profile.*` and `dml32_encode.*`. The encoder produces an
integer bandwidth input, timing clock requirements, four independently
identified watermark calculations, transition constraints and the full surface
mode. Its API explicitly remains an in-process userspace bridge, not an IPC
ABI. There is no native import/validation boundary or retained calculation
object, and `displaybandwidth::Io` explicitly has no native implementation.

Implement that boundary before a callable native timing-enable API. Bind the
validated record to the actual device/reset generation, selected advertised
timing and ROM connector route, complete surface/pitch/allocation, private clock
transactions and current mode generation. Preserve the independently calculated
A/B/C/D sets. Numeric IDs and copied results do not authenticate a calculation.
Derive OTG global-sync values from this same retained integer plan, never from
EDID alone or a second caller-supplied structure. Kernel code must not execute
the userspace floating-point DML engine.

## Preparation and enable dependencies

1. Under the real exclusive mode transaction, validate the mode/profile and
   retain all actual calculation and route inputs. The first portable profile
   is progressive single-pipe linear RGB32 storage, RGB24 output, direct VRAM,
   with no DSC/ODM/MST/UHBR. Revalidate link capacity and the actual sink mode.
2. Establish required firmware exclusion and genuine clock transactions while
   the hardware is cold. Native global DISP/DPP requests require no surface
   lease, queues, VM owners, links, streams or DTOs. The DML ceiling tuple is a
   requirement, not proof that all required clocks are held simultaneously.
   Include the DCF/SOC/fabric/DRAM and transition requirements in the same owner.
3. Retain the selected pipe power/DTO, create and fully initialize the real
   surface, and configure the blanked HUBP/DPP/MPC/OPP path. Then program real
   RQ/DLG/TTU, DET/compbuf and global A–D watermarks through a native
   `displaybandwidth::Io` binding. That module owns all four DET allocations and
   global watermarks together, not an independent per-pipe fragment.
4. Bind the exact pixel-clock transaction to the mode/path/PLL and epoch.
   `Display::pixelClock` currently sends a DMCUB command; it does not retain an
   exact native pixel-clock ownership record or independent programming
   readback. Its transport completion cannot supply this missing lease.
5. With those owners retained, configure OTG only while the selected master is
   stopped and not busy, with exclusive OPP input routing and no foreign locks.
   Portable `configure` programs timing, global sync, clock controls and 1:1
   ODM bypass and verifies their readback. `enable` enables VTG before master,
   observes actual enabled/clocked state and an advancing frame count, and
   rejects underflow. This proves timing activity, not pixel fetch or sink lock.
6. Start fetch and the physical stream in their source-backed order under the
   same retained mode transaction. HDMI and DP stream controllers require the
   selected OTG running. DP's stream disable must observe actual disabled
   status before asserting steer-FIFO reset; a failed blank retains ownership.

There is an important current ordering constraint: `prepareBlanked` verifies
the private DISP/DPP records **before** connecting MPC. Their verifier calls
`displayidle::verify`, which requires every OTG stopped and MPC disconnected.
It cannot be reused after our own route is connected or timing is running.
Add an active-safe verifier tied to the exact private mode/pipeline leases and
fresh clock/firmware observations; preserve the existing cold admission for
clock mutations. Do not relax the idle masks or treat owned generation alone
as proof that current hardware still matches.

## Ordered normal retirement

Stop/mute the actual stream before withdrawing timing or its link clock.
Blank and drain the owned HUBP while its clocks remain available. Then stop
OTG/VTG and wait actual master-off/BUSY=0, disconnect the pixel route, and retire
the surface allocation only after all its memory references are stopped.
The existing `pixelpipeline::stop` rejects an active OTG, so timing retirement
must precede it. Release bandwidth programming only under its required global
blank/drain condition. Retire DTOs, pipe power, clock policies and firmware only
after their actual consumers have relinquished them.

Keep TMDS symbol-clock dependency explicit: upstream drops the OTG symbol-clock
reference after CRTC/OPTC clock disable and may defer physical link clock-off
until then. A generic PHY-off-before-OTG sequence is not justified. Any ambiguous
write, stop failure or revoked gate retains the hardware hold and all dependent
owners; starting a fresh controller must not erase this history.

## Existing reset-release finding

`ResetController::verifyRetirement` requires MODE1, native ASIC_Init, BME off,
and actual stopped memory-client observations. Its scanout stop explicitly
does **not** stop OTG/MPC/DIO/PHY. No later readback closes that downstream gap.
Nevertheless `Display::releaseAfterDeviceReset` currently clears native HDMI
streams, DIO links, DTOs and pipe-power records and drops its lease/self-hold
after only GART access and absence of a surface. Engine validates the genuine
memory-client proof before calling it, but the Display method itself accepts
neither that proof nor the retirement scope.

This is an ownership-contract hole for retained downstream owners. It is not
evidence that those blocks actually survive MODE1 on this target, and not a
demonstrated DMA use-after-free: the separate proof checks memory clients.
Current startup boots DMCUB but has no native callers enabling these physical
APIs, so the downstream case is presently latent. Upstream Linux's reset path
commits zero streams before reset and rebuilds assignments on resume; its
generic full-reset label is not proof of our omitted post-reset checks.

The minimal safe correction is to require the authentic scope/proof at the
Display release boundary and reject release, without erasing state, while any
native link, touched stream, DTO/power owner or AUX/DDC owner remains. Validate
the proof against the actual provider/owner/map before taking Display locks to
avoid Reset/GART lock inversion. This preserves the DMCUB-only release path.
A subsequent real downstream retirement protocol can replace the rejection;
neither cold `displayidle` (which omits PHY/VPG) nor a caller boolean suffices.
The future timing owner must join these checks and hardware holds.

Validation should cover wrong/revoked proof before mutation, each retained
owner category preserving the lease/journals on failure, genuine DMCUB-only
release, and stable references across repeated rejection. Native header
compilation is additionally required. No tests or hardware runs were performed
for this read-only review.

## Evidence

Local implementation: `include/hackynavi/{otg,display_bandwidth,display_idle,
boot_display_stop}.hpp`, their portable implementations, native
`HackyNaviScanoutSurface`, `HackyNaviDisplayClockControl`,
`HackyNaviDisplayController`, `HackyNaviResetController`,
`HackyNaviEngineController` and `HackyNaviLifecycleController`.

AMD/Linux pinned revision `3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`:

- [DCN3.2 OTG enable/disable and ODM bypass](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/optc/dcn32/dcn32_optc.c):
  `dcn32_enable_crtc`, `dcn32_disable_crtc`, `dcn32_set_odm_bypass`.
  Local SHA-256 `7108d3181cb2c8f72f24fc6cfab6b09360fb8122b50db8647dcf5c1729c2901f`.
- [Display sequencing](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/hwss/dcn20/dcn20_hwseq.c):
  `dcn20_enable_stream_timing`, `dcn20_prepare_bandwidth`,
  `dcn20_reset_back_end_for_pipe`.
  Local SHA-256 `7ff3b41274c28f374dea50d87c541e49b71e2a0fb5ddc93eed500810f3910cc5`.
- [DCN3.2 bandwidth preparation](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/hwss/dcn32/dcn32_hwseq.c):
  `dcn32_prepare_bandwidth` and FAMS coordination.
  Local SHA-256 `2907433372340405f9c3f83daf57d96bd441d5671a6faa43b1d2cb4cee3d3539`.
- [Display reset suspend/resume](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/amdgpu_dm/amdgpu_dm.c):
  `dm_suspend` commits zero streams; `dm_resume` reconstructs the display state.
- [Full GPU reset recovery](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu/amdgpu_device.c):
  `amdgpu_device_reinit_after_reset` reposts ASIC and resumes IP blocks. This is
  evidence of the recovery architecture, not a downstream stopped-state check.
