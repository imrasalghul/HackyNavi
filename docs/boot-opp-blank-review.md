# Independent OPP blanking review

The new `bootopp::Controller` matches the single-OPP RGB part of Linux
`dcn32_init_blank` at revision `3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`.
It remains disconnected from native hardware until an actual inherited-display
owner supplies exclusive lifetime and access guards. This review and its tests
do not establish blanking on the installed GPU.

The source chain reviewed is `dcn32_init_blank` →
`optc1_get_otg_active_size` and `opp2_set_disp_pattern_generator` →
`opp2_dpg_set_blank_color`, followed by the DPG pending/blanked checks. The
upstream helper first writes dimensions and zero offset, writes the two banks
of blue/green/red black values, changes only EN and MODE to horizontal bars,
then writes dimensions again. The `hw_shared.h` enum defines horizontal bars
as mode 4. Equal colors produce uniform black. The implementation preserves
the unrelated admitted DPG control fields exactly, as the source RMW does.

Active dimensions are the enabled OTG's blank-start minus blank-end values.
The implementation rejects wrapped, zero, reserved-bit and too-large values
before writing the narrower 14-bit DPG fields. OPP and OTG instance numbers
come independently from the actual backend capture; neither is assumed equal
to the link, MPCC or DPP instance. The first profile rejects multiple ODM
sources instead of applying the upstream two-OPP split-width branch.

The focused runner independently checks all 52 instance register addresses
and base indices against the generated DCN3.2 header, the dimension/blanking
field masks and shifts, OPP clock mask, allowed DPG mask, rejected FMT fields,
and actual horizontal-bars enum. It compiles the production translation unit
against the real kernel headers with a 2 KiB frame warning treated as an error.

The numeric tests run the production controller and backend parser. They
check the complete seven-write order and repeated dimensions, preserved
control fields, independent OPP/OTG selections, 1×1, 3840×2160 and maximum
encodable extents. Rejections cover malformed discovery/ROM, aliases between
backend reads and OPP writes, insufficient BAR range, unsupported link,
pending/test-pattern/format/clock state, invalid dimensions and missing access
callbacks. Every potentially delivered write is tested both as an unlanded
failure and an ambiguously landed failure, plus every immediate readback
mismatch. Selected actual guard, initial-read, post-write and final-read
positions exercise all-ones, transport failure and access denial. Poll
timeout, revocation after delay, final pending state, raw clock change and a
still-classifiable changed backend all remain retained failures. Failed or
completed controllers cannot replay the transaction.

No production edit was made by the independent reviewer. No concrete source
or protocol defect was found in the reviewed version. `Blanked` proves only
the observed DPG black state under the caller's exclusion; it does not prove
HUBP drain, HDMI shutdown, MPC detachment, clock ownership or DMA retirement.
