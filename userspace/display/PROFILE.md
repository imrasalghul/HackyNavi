# Captured DCN3.2 model inputs

`dml32_profile.cpp` connects board and firmware observations to the real DML32
calculator. It is a trusted, in-process CPU adapter. It is not a kernel wire
interface, a clock lease, or evidence that a mode has appeared on a display.

The native `HackyNaviDisplayController::captureDpmInventory` now retains one
generation-bound observation containing all of these inputs:

- The selected, immutable legacy ROM's PCI/subsystem identity, FirmwareInfo
  revision, DCE4.5 crystal/I2C references and raw latency overrides, and VRAM3.0
  channel topology. The table's alternative memory modules are not added
  together. Channel width is `(1 << channel_width) / 8` bytes.
- Seven independent DAL DPM tables, their raw metadata/frequency words, and
  their DC ceilings from `GetDcModeMaxDpmFreq`. Two entire observations must
  agree. Discrete tables and fine-grained min/max tables retain their distinction.
- Two identical reads of `CLK1_CLK_PLL_REQ` and the enabled DCHUB global timer.
  The VCO is derived from the programmed multiplier with the pinned DCN32
  100 MHz DFS reference. DCCG follows the source's XTAL contract; this supported
  board parser requires a 100 MHz ROM crystal. The enabled timer divider gives
  DCHUB's reference. These are source-backed derivations from programming and
  ROM, **not independent measurements of running clocks or a verified clock
  source selector**. No default VCO substitutes for a zero/failed register read.

Capture runs under the actual normal FunctionGroup scope and installed GART
access guard, with the same measured deadline before and after its hardware
accesses. The native owner retains the ROM, discovery, BAR map and private
record. Revocation, a changed generation, clock-minimum changes, stop or reset
invalidate the observation. Copying its CPU fields does not transfer that
authority to another process.

## Initial calculation policy

Call `displayprofile::build` or `displayprofile::calculate` with the explicit
`ColdFixedDcCeilingV1` policy. It constructs one **synthetic requirement tuple**,
specializing Linux's inserted all-max-DC state for the initial fixed-clock,
single-pipe path. There is no claim that firmware table index N denotes the
same voltage state across different clocks.

Each clock uses its greatest reported legal value within its own DC ceiling.
Discrete clocks select an actually reported level; fine-grained clocks may
select a ceiling between reported min/max. DISPCLK and DPPCLK also obey AMD's
1950 MHz DCN3.2 cap. UCLK MHz converts to DRAM MT/s by 16. SOCCLK remains an
independent requirement. The result's state index zero is a calculation index,
not a measured voltage. A native consumer must establish and verify all clock
requirements together before using any resulting register plan.

`dml32_asic.c` contains the pinned DCN3.2 hardware capacities and AMD's explicit
latency/derating policy. It contains no spreadsheet clock, memory topology or
VCO defaults. The policy's PHY ceilings describe link capabilities, not running
PHY clocks. MALL capacity is zero because this profile excludes MALL. The
single RGB plane uses source-backed prefetch ratio four and a 60 us minimum
strobe prefetch policy.

The ROM supplied for this project has zero MCLK/self-refresh latency overrides.
Zero selects the named ASIC policy; it is not interpreted as zero latency.
Nonzero overrides currently fail profile construction: the pinned ATOM header
says 100 ns units, while `dcn32_update_bw_bounding_box_fpu` multiplies these
values by 10 to populate microseconds. The adapter preserves the raw fields
and refuses to silently choose between those interpretations.

`Mode` supplies exact timing, allocation pitch and real link limits. The adapter
constructs one progressive, unscaled RGB32/direct-VRAM pipe with SDR24 output.
It supports the current legacy DP/TMDS calculation profile; HDMI FRL/DSC and
the final 4K165 target need their respective implementations. It never assumes
an absent HDMI EDID ceiling permits 600 MHz.

The trusted process calls the actual `hn_dml32_calculate` and returns its real
status. AMD's arithmetic assertions remain fatal; this API is not an untrusted
IPC input boundary. A later service must have process isolation, bounds and a
timeout. The adapter does not invent four watermark policies, copy set A into
other slots, or authorize clock transitions/stutter.

## Validation and remaining connection

`build-test.py --sanitize` tests clock selection, malformed/stale numeric
representations, ambiguous latencies, mode construction and actual DML32
calculations for 1080p60/4K60. The observations supplied by these tests are
explicitly synthetic. `tests/display_model_input_tests.cpp` separately tests
ROM/discovery bounds and every reference-read failure/revocation/deadline path;
an optional ROM path exercises real board parsing without hardware access.

Native captured inputs have not yet been obtained from the target GPU through
this new path. The authenticated native producer/consumer, retained calculation
records, all required clock holds, watermark/transition policy, and bandwidth/
OTG/scanout modeset transaction remain to be connected. No calculation output
from this adapter is hardware authorization.

## Pinned primary sources

All links below use Linux commit `3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`.

- [DCN32 ASIC model, synthetic states, ROM overrides and prefetch policy](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/dml/dcn32/dcn32_fpu.c): lines 64–196, 2775–3005, 3078–3124, 3466–3483.
- [Firmware tables, DISP/DPP cap, PLL derivation and DFS reference](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/clk_mgr/dcn32/dcn32_clk_mgr.c): lines 132–153, 211–256, 864–885, 1043–1065, 1186–1207.
- [DAL DPM/DC-limit protocol](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/clk_mgr/dcn30/dcn30_clk_mgr_smu_msg.c): lines 245–292.
- [DCN32 DAL message IDs](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/clk_mgr/dcn32/dalsmc.h) and [PPCLK IDs](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/clk_mgr/dcn32/dcn32_smu13_driver_if.h).
- [ATOM firmware parser](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/bios/bios_parser2.c): DCE4.5 crystal and VRAM3.0 channel-width decoding.
- [ATOM table layouts and latency units](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/include/atomfirmware.h): `atom_display_controller_info_v4_5` and `atom_vram_info_header_v3_0`.
- [DCCG XTAL contract](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/dccg/dcn32/dcn32_dccg.c) and [DCHUB timer-divider derivation](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/hubbub/dcn20/dcn20_hubbub.c).
- [DCN32 resource policy](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/resource/dcn32/dcn32_resource.c): `min_prefetch_in_strobe_ns = 60000`.

AMD's MIT notice is retained in the adapted ASIC-policy and reference-acquisition
sources. The vendor DML tree remains unchanged and manifest-verified.
