# Display calculation source and licensing

The files under `vendor/dml` are unmodified portions of AMD's Display Mode
Library from the Linux source tree. Each file carries AMD's MIT permission
notice and copyright. The source manifest pins the repository revision,
upstream path, byte count and SHA-256 of each file. In particular,
`calcs/dcn_calc_math.h` originates at `dc/inc/dcn_calc_math.h`.

The test-only `tests/reference_profile.h` preserves the AMD notice and copies
the DCN3.2 IP/SoC initializer values from `dc/dml/dcn32/dcn32_fpu.c` at the same
revision, with declarations renamed/static/const. Its defaults are model test
inputs, not measurements of the user's GPU.

`dml32_asic.c` separately adapts that pinned source's ASIC capacities and
latency/derating policy into production calculation inputs. It retains AMD's
MIT notice and deliberately omits spreadsheet clocks, topology and VCO defaults.
Those fields must be supplied by the captured-input adapter. `PROFILE.md`
documents the exact policy and the difference between programmed values and
running-clock measurements. The portable `src/display_model_input.cpp` retains
AMD's notice for its source-backed ROM/reference-clock adaptation.

Original compatibility, adapter, build and test code is MIT-licensed under the
repository license. The DCN32-only dispatch in `dml32_runtime.c` implements
the initialization sequence in AMD's `display_mode_lib.c`; it excludes other
ASIC implementations. No Linux driver, allocation, power-management or MMIO
operations are replaced with simulated successes. This component performs CPU
calculations only and is deliberately excluded from kernel compilation.

Upstream reference:
https://github.com/torvalds/linux/tree/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/dml
