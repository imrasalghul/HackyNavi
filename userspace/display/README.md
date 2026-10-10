# DCN3.2 display calculations

This userspace component executes AMD's real DML32 formulas to calculate the
bandwidth, clocks, global-sync timing and DLG/RQ/TTU settings for a proposed
display mode. It does not map registers or program a display.

The initial adapter supports one progressive, unscaled RGB32 linear surface,
direct VRAM addressing, and an SDR RGB24 HDMI or legacy DisplayPort output.
DCC, GPUVM, MALL, DSC, writeback, cursor, immediate flip, ODM and MPC splitting
are explicitly excluded from this adapter. These are implementation limits,
not a reduction of the project's display goal.

## Build and validation

From the repository root on this Windows workspace:

```powershell
python userspace/display/build-test.py --sanitize
```

With an existing native Clang on macOS:

```sh
python3 userspace/display/build-test.py --compiler "$(xcrun --find clang)" --sanitize
```

The script verifies every vendor file against its source manifest and always
invokes the compiler. It writes exact source hashes, command results and a log
under `build/display-dml`. Tests calculate 1080p/2160p HDMI and four-lane DP
plans, check the response to changed memory latency, and reject invalid clocks,
NaN, unsupported VM/DCC/cursor inputs and unsafe arithmetic/array bounds.
Address, undefined-behavior and float-cast-overflow sanitizers are enabled by
`--sanitize`. Test clocks are explicitly a reference fixture, not target data.

The same build now runs `tests/encoding.cpp`: real calculations pass through
`dml32_encode.cpp` into the integer `displaybandwidth::Input` and kernel plan
validator. No output is a target modeset result. Real clock holds, the trusted
native calculation bridge and modeset programming remain.

`dml32_profile.cpp` now builds a real DML input from the native owner's captured
ROM, DAL DPM/DC-ceiling tables and PLL/timer observations. It uses an explicit
fixed-clock DCN3.2 ASIC policy, with no spreadsheet clock/topology/VCO fallback.
The tests exercise this path with modeled observations; no target clock capture
or modeset is claimed. See [PROFILE.md](PROFILE.md) for provenance and limits.

## Integer register encoding

`dml32_encode.hpp` binds a timing calculation and four explicitly supplied
watermark calculations to device, mode and clock-table generations. All sets
must describe the same surface geometry, output and measured reference clock;
each set has a distinct retained calculation ID. The adapter maps all 70 named
RQ/DLG/TTU fields explicitly, converts microseconds/fractions to integer
nanoseconds/per-thousand using upstream truncation, and checks the resulting
register plan against exact hardware masks. Missing or failed calculations,
changed generations, incompatible modes, nonfinite values and overflow clear
the output and return an error.

Required clock minima round upward to kHz; DRAM retains ktransfers/s rather
than being mislabeled as UCLK. Each watermark set keeps its own clock and
transition requirements. The adapter does not select firmware A/B/C/D policies,
copy set A into other slots, or authorize memory-clock transitions. Native
ownership must retain and check the full mode/clock records under the common
operational gate. This is an in-process interface, not an untrusted wire ABI.

The integer controller in `src/display_bandwidth.cpp` now programs and verifies
39 timing registers, four nine-register watermark sets and bounded DET/compbuf
changes under explicit operational-gate callbacks. It requires stopped OTGs,
blank/drained powered HUBPs and the same direct-VRAM profile. It never starts
scanout. Host protocol tests and real kernel-header compilation passed; the
native bandwidth binding and modeset transaction remain to be connected.

The user's display target is Samsung Odyssey G90XF, 3840x2160 at 165 Hz, working
through either connector and currently connected by HDMI. A read-only Windows
capture reports a 1,629,250,000 Hz pixel clock. The current legacy-link adapter
does not support that timing: HDMI FRL, DP DSC, and any required pipe splitting
must be implemented and validated. The 2160p fixture below remains a 60 Hz test.

## Input and output contract

`hn_dml32_calculate` takes a populated board/firmware SoC bounding box, DCN3.2
IP description and exact pipe parameters. It does not fill absent clock data
with spreadsheet defaults. The IP buffer limits are ASIC parameters; clocks,
memory configuration and firmware latency inputs must have explicit provenance.
The test-only `reference_profile.h` is not linked into the adapter library.

The reference clock must be the observed DCHUB timer frequency, including its
divider, rather than an assumed crystal frequency. Output includes required
DISPCLK, DPPCLK, DCFCLK, SOCCLK, fabric clock, DRAM transfer rate and deep-sleep
DCFCLK, along with the model's transition-support results, urgent latency and
nominal/immediate-flip urgent bandwidth fractions. The selected-state DTB
reference is informational: `dtbclk_required` is false for this legacy-DP/TMDS
profile. A native owner must
establish and maintain those clock requirements before using the register plan.
The Linux conversion from DRAM MT/s to UCLK MHz is division by 16.

The wrapper first validates the mode, rejects a required split, selects the
validated DCFCLK state, recalculates with actual required display clocks, then
obtains watermarks and global-sync values before DLG/RQ/TTU extraction. The
linear single-pipe profile requests 1152 KiB DET and leaves 128 KiB compbuf.

Non-DSC DML32 auto-selects link bpp, including lower values than the requested
24. The adapter separately checks exact RGB24 payload capacity at the supplied
DP rate/lane count/downspread and selected PHY clock, or the HDMI TMDS clock
limit. The 594 MHz test on two HBR3 lanes is rejected; four lanes pass. These
are test inputs, not observations of the user's current Windows link. Actual
sink, cable, training and board limits remain the native link owner's inputs.
For this no-DSC legacy-DP/HDMI profile, upstream OutputBppPerState affects link
admission but does not feed exported WM/RQ/DLG/TTU values: DSC delay is zero,
and RequiredDTBCLK's link-bpp use is restricted to the excluded FRL profile.

Two unused-path arithmetic inputs still need nonzero values in unmodified
DML32: GPUVM page granularity and DSC slice count. The API requires at least
4 KiB and exactly one
slice while both features remain disabled. Zero values are rejected, not
silently replaced; sanitizer tests caught the unconditional slice calculation.
The sentinel clock slot also means `num_states < DC__VOLTAGE_STATES`.

The adapter normalizes unused requestor fields after extraction: luma VM-group
encodings and all second-plane RQ fields become zero. In this strictly no-VM,
single-plane profile those requestors are disabled. Unmodified DML32 returns
zero group bytes/no chroma swath, then unconditionally log2-encodes them into
wrapped unsigned values. Those values must not be masked into hardware fields.
Active luma geometry and timing are untouched; the native integer plan must
still reject any overflowing active field and enforce the same hardware profile.

This is an internal trusted-profile API using upstream structures, not an IPC
or kernel ABI. It validates supported geometry and finite numeric inputs but
does not claim complete validation of arbitrary upstream structures. Upstream
assertions remain active and terminate the process on violated invariants.
An external mode-request service needs a bounded process boundary and a typed
integer response tied to the device's power/configuration generation.

The calculated watermark set covers the supplied state. Linux's firmware
dependent B/C/D policies, dynamic memory switching, multimonitor planning and
driver-controlled display output remain to be integrated. Success is never
reported as GPU execution or display ownership.

## Provenance

Twenty unmodified MIT-licensed AMD source/header files are recorded in
`vendor/source-manifest.json`, from Linux revision
`3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`. Every file retains its AMD notice.
`dml32_runtime.c` supplies DCN32-only dispatch, standard memory/types,
optional real diagnostic logging and active assertions for userspace.
The formulas and their float-rounding helpers are unchanged. See `NOTICE.md`.

One source inconsistency requires separate review when binding real ROM data:
ATOM fields documented in 100 ns units imply multiplication by 0.1 for
microseconds, while the pinned Linux DCN32 override multiplies them by 10.
Preserve raw ROM values and their units; this adapter does not choose or apply
either conversion on behalf of the board-data acquisition layer.
