# DCN3.2 programmed-clock capture and cold DAL policies

The native Display owner now has real one-shot DAL UCLK hard-maximum and DCF
deep-sleep-floor entry points. They retain the actual normal FunctionGroup scope,
Display/Firmware/GART locks, a single elapsed deadline and the existing DMCUB
coordination checks. Prepared DMCUB requires actual disabled/reset readback;
running DMCUB requires this owner's completed FAMS-disable and DAL handoff.
Both paths exclude queues, VM owners, scanout, links, streams, DTOs and retained
AUX/DDC access before writes, and verify physical pipe idle.

Each request queries stable real firmware tables and DC ceilings. UCLK must be
an advertised level or lie within an advertised fine-grained interval. A DCF
sleep floor is a MHz requirement, not a normal DPM level index; it must fit the
actual DCF table maximum and DC ceiling. The owner is retained before mailbox
writes. A private journal records possible delivery, ACK and raw reply separately.
Neither command has a sourced fulfillment bit or measured-frequency reply.

The maximum cannot lie below an already owned memory minimum; a later minimum
cannot exceed the owned maximum. Invalid combinations reject before native
queries and serial changes. Repetition of these one-shot requests is rejected.
The existing scope-aware boot checks now cover both added policies and require
their acknowledged, completely read replies and the same retained generation.
Normal release requires verified reset for these DAL policies; zero MHz is not
presented as a proven withdrawal. There is no startup or modeset caller yet.

## Divider calculation and actual register observations

`displaydentist::plan` uses the pinned DCN3.2 integer preset calculation and the
piecewise DID conversion. It returns the floored MHz mailbox request and the
frequency predicted from the resulting DID as separate fields. Impossible or
out-of-profile requests reject rather than saturating to a plausible result.
This is arithmetic; it cannot establish a clock request or a running frequency.
The special divider-127 transition still requires its separate divider-126
programming step before this plan can be used by a complete DISPCLK owner.

`captureProgrammedClocks` reads the real Navi31 PLL, five DFS slice registers and
discovered DCN3.2 DENTIST register twice. All fourteen reads must agree and the
PLL must still match the Display owner's retained board/reference observation.
Every read is bounded by the actual scope/deadline. Invalid DID values are
retained only as raw observations; they never clamp into a fabricated frequency.
Required clocks must decode, while optional disabled clocks may remain invalid.
Change-done flags and DISP divider agreement are reported separately.

These values describe programming derived from the PLL and divider registers.
They are not frequency-counter measurements, DAL fulfillment, a continuous
hold, or DML authorization. Capture changes no hardware. Output becomes visible
only after a complete capture and final native physical-idle/scope checks.

The first strict and UBSan local suites each passed 293,009 arithmetic and
transport fault checks. They cover all DID ranges, clock ceilings, representable
rounding, every read/revocation/deadline boundary, stable mismatches and optional
clock handling. The native control object compiles with real kernel headers and
a 2 KiB stack-frame limit. Native validation subsequently passed in
`build/native-display-clock-integration-final`: 293,009 dentist and 13,443 DAL
ASan/UBSan checks, plus eight real kernel object compiles. Root verified all
88 archived inputs and native object hashes; the 2,120 KiB stage was removed.
Independent product and VCN reviews passed. The complete 97-unit kext in
`build/macos-coordinated-clocks-final` includes these changes and resolves all
412 imports. No hardware clock operation has run.

Primary code is pinned to Linux
`3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`:
[DCN3.2 clock manager](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/clk_mgr/dcn32/dcn32_clk_mgr.c),
[DID decoding](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/clk_mgr/dce100/dce_clk_mgr.c),
[DID encoding](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/clk_mgr/dcn20/dcn20_clk_mgr.c),
and [divider constants](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/inc/hw/clk_mgr_internal.h).
