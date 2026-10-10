# Inherited OPP solid-black transition

`bootopp::Controller::blank` performs a one-shot solid-black transition for the
observed single-OPP RGB HDMI-TMDS profile. It captures the backend itself,
derives the selected OPP and OTG independently, reads the real OTG active size,
and programs DPG dimensions, zero offset, both RGB color banks, horizontal-bars
enable and repeated dimensions. It then waits for DPG pending to clear and
rechecks the backend, controls, clocks and dimensions.

This follows Linux revision `3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`:
`dcn32_init_blank`, `optc1_get_otg_active_size`,
`opp2_set_disp_pattern_generator`, `opp2_dpg_set_blank_color`,
`opp2_dpg_is_pending`, `opp2_dpg_is_blanked`, and the horizontal-bars enum in
`hw_shared.h`. The seven-write sequence includes the source's initial
dimensions/offset writes and its repeated dimensions write after enabling.

The caller must own the real display operation and exclude firmware, modeset,
clock, reset and PM changes for the full call. Every access has live permission
and operation guards. Pixel clocks and full raw relevant state are checked
before each write. The poll delay budget is at most 100 ms; the native owner
must also enforce one elapsed deadline covering reads and writes. The
controller should live in its native owner's heap, retaining its journal on
failure. No native hardware callsite is connected yet.

Journal state advances before any potentially delivered write. Failure seals
the one-shot controller; success cannot be replayed either. Only an observed
black DPG is reported. HUBP drain, audio/HDMI/transmitter shutdown, MPC/OTG stop,
clock changes and DMA release remain separate owned operations.

The focused tests and generated-header oracle are run by
`scripts/check-boot-opp-blank-local.py --output build/<fresh-directory>`.
They test real production protocol code with numeric register callbacks and
compile against actual kernel headers. They do not run on or emulate the GPU.
The independent findings and coverage are recorded in
`docs/boot-opp-blank-review.md` and the generated hash-bound validation report.
