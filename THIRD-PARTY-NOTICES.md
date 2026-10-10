# Third-party code notices

## Cobalt64

The PCI BAR mapping and MMIO access design in
`kext/HackyNavi/HackyNaviDevice.cpp`, and MM_INDEX transport in
`src/device_discovery.cpp`, adapt portions of
[Navi48Bringup.cpp](https://github.com/Almosst-DEV/Cobalt64/blob/d6c9aedf8e6526a39e349b5886cbc8c7ddf77f65/src/navi48-bringup/src/Navi48Bringup.cpp)
from Cobalt64 revision `d6c9aedf8e6526a39e349b5886cbc8c7ddf77f65`.
The selector/data upload protocol in `src/vram_transfer.cpp` also adapts
[`amd/amdgpu_regs.h`](https://github.com/Almosst-DEV/Cobalt64/blob/d6c9aedf8e6526a39e349b5886cbc8c7ddf77f65/src/navi48-bringup/src/amd/amdgpu_regs.h)
at that revision, with allocation-bound access and full readback added here.
HackyNavi adds its own Navi 31 selection, bounds, ownership, acquisition and
validation logic. This notice applies to the adapted portions, not to all
components of the upstream project. No Cobalt64 firmware or Metal translator
is included in this repository.

MIT License

Copyright (c) 2026 Navi48-MacOS contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Linux AMD GPUVM, memory and engine protocols

`src/display_clock.cpp` adapts DCN3.2 per-pipe DTO/divider programming from
`dcn20_dccg.c` and `dcn32_dccg.c`. `src/display_smu.cpp` adapts the DAL mailbox
and clock-minimum completion protocol from `dcn32_clk_mgr_smu_msg.c` and the
DCN3.2 `dalsmc.h`. These use the pinned Linux revision below and retain AMD's
MIT notices; the port adds stricter validation and bounded failure handling.
`src/display_idle.cpp` uses the DCN3.2 register definitions and the disconnected
selector/idle conditions from `display/dc/mpc/dcn10/dcn10_mpc.c`, retaining its
AMD MIT notice.

`src/display_power.cpp` adapts DOMAIN0..3 and IP_REQUEST_EN sequencing from
`dcn32_hwseq.c`, `dcn20_hwseq.c` and `dcn10_hwseq.c`. `src/hubp_scanout.cpp`
adapts HUBP surface, address, flip and blank/drain protocols from
`dcn10_hubp.c`, `dcn20_hubp.c`, `dcn30_hubp.c` and `dcn32_hubp.c`.
`src/pixel_pipeline.cpp` adapts DPP/CNVC/DSCL, MPC and OPP programming from
the DCN10/20/30/32 DPP, MPC and OPP implementations, including
`dcn10_dpp_dscl.c`. All three use generated DCN3.2 register definitions at
the pinned Linux revision and preserve AMD MIT notices in source. These ports
add bounded validation and ownership handling; they do not import DML policy.

`src/hdmi_stream.cpp` adapts HDMI packet scheduling and FIFO sequencing from
`dcn30_dio_stream_encoder.c` and `dcn32_dio_stream_encoder.c` at the pinned
Linux revision, retaining AMD's MIT notice.

`src/vpg.cpp` adapts the DCN3.2 packet-memory sequence from AMD's
`display/dc/dcn30/dcn30_vpg.c` and generated register definitions at the Linux
revision below. It retains the original AMD MIT notice.

`src/gpu_vm.cpp` adapts GMC11 page-table flag definitions and
translate-further rules from AMD's `amdgpu_vm.h` and `gmc_v11_0.c` in
[Linux revision 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0](https://github.com/torvalds/linux/tree/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu).
`src/gart.cpp` adapts GFXHUB 3 / MMHUB 3 enable, disable and invalidation
sequences from `gfxhub_v3_0.c`, `mmhub_v3_0.c` and `gmc_v11_0.c` at that revision.
`src/hdp.cpp` adapts NBIO 4.3 remap setup and the CPU HDP flush protocol from
`nbio_v4_3.c`, `amdgpu_hdp.c` and `hdp_v6_0.c`.
`src/boot_display.cpp` adapts DCN3.2 register definitions and HUBP address,
format and pitch interpretation from `dcn_3_2_0_offset.h`,
`dcn_3_2_0_sh_mask.h`, `dcn20_hubp.c`, `dcn30_hubp.c`, `dcn30_hubbub.c`
and `dcn32_resource.c` at the same revision.
`src/bootstrap_quiesce.cpp` adapts CP/SDMA stop sequences and register
definitions from `soc21.c`, `gfx_v11_0.c`, `sdma_v6_0.c`, and the GC11,
MP13 and MMHUB3 generated register headers at the same revision.
`src/mode1_reset.cpp`, `src/nbio.cpp`, `src/psp.cpp`, `src/psp_ring.cpp`,
`src/sdma_firmware.cpp`, `src/sdma_queue.cpp`, `src/gfx_firmware.cpp`,
`src/smu.cpp`, `src/gfx_init.cpp` and `src/gfx_queue.cpp`
adapt the corresponding AMD reset, NBIO, PSP, firmware, power and queue protocols
from the same pinned tree. `src/gfx11_clearstate_data.hpp` preserves AMD's
complete GC11 clear-state table and its original copyright and MIT notice.
`src/dp_aux.cpp`, `src/dp_training.cpp`, `src/hdmi_ddc.cpp`, `src/hdmi_link.cpp`, `src/dio_link.cpp`, `src/dmcub.cpp`
and `src/dmcub_firmware.cpp` adapt the DCE AUX, DCN3.2 GPIO/I2C/link-encoder
and DMCUB firmware protocols. `src/otg.cpp` adapts the DCN single-pipe
OTG/VTG/ODM timing sequence from `dcn10_optc.c`, `dcn30_optc.c`,
`dcn32_optc.c` and the DCN3.2 register headers at the same revision. `src/runtime_rom.cpp` adapts live SMUIO/NBIO ROM
acquisition, and `src/cold_aux.cpp` adapts VCN/JPEG/LSDMA power/idle admission.
`src/cold_dma.cpp` adapts IH/DWB/MCIF/MALL register definitions and idle checks;
`src/boot_display_stop.cpp` adapts the DCN HUBP blank/drain protocol.
`src/atom_vm.cpp` adapts `atom.c` and `atom.h` from that revision, with the
original AMD 2008 notice and author attribution retained in the source.
Each implementation records its source scope
and applicable notices in the source file.
The bounded encoding, planning, ownership and validation APIs are implemented
for HackyNavi.

Copyright 2012-2025 Advanced Micro Devices, Inc.
Copyright 2008 Red Hat Inc. Copyright 2009 Jerome Glisse.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

## AMD GFX11 compute shaders and Mesa command definitions

`src/gfx11_compute_data.hpp` contains AMD's GFX11 buffer-clear and buffer-copy
machine code from `tests/amdgpu/shader_code_gfx11.h` in
[libdrm 2.4.120](https://gitlab.freedesktop.org/mesa/drm/-/tree/75254bf2390c10644ffb35a90fc8f18f196f9f0c),
revision `75254bf2390c10644ffb35a90fc8f18f196f9f0c`. That file retains AMD's
complete 2022 copyright and MIT notice. The dispatch setup in
`src/gfx_compute.cpp` uses that revision's `shader_test_util.c` register and
descriptor definitions. These are real shader instructions, uploaded to
owned VRAM by the kernel queue; test code is not linked into the kext.

Compute preamble, packet, cache and instruction-prefetch definitions use
`ac_cmdbuf.c`, `ac_cmdbuf_cp.c`, `ac_cmdbuf.h`, `ac_barrier.c`, `ac_binary.c`
and AMD register tables in
[Mesa revision 35b085c4c067f198ce0b6437d94678e47b9c645f](https://gitlab.freedesktop.org/mesa/mesa/-/tree/35b085c4c067f198ce0b6437d94678e47b9c645f/src/amd).

Copyright 2012, 2022, 2024 Advanced Micro Devices, Inc.
Copyright 2024, 2026 Valve Corporation.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

## Userspace AIR shader compiler

`userspace/metal/vendor/metal2vulkan` contains the unmodified translator
library/CLI source subset from `translator/translator` at NVIDIA reference
commit `508f3d2496e78a9640d59128a56ddb5a53061606`. Its package attributes
metal2vulkan to Anees Iqbal and contributors and identifies the original
repository as `https://github.com/steelbrain/metal2vulkan`.

This translator is **LGPL-3.0-or-later**, separately from HackyNavi's MIT code.
The complete LGPL and GPL license texts are retained in its `LICENSE` and
`COPYING`; per-file provenance is in `userspace/metal/vendor/source-manifest.json`.
The NVIDIA project's PolyForm driver/frontend implementation is not included.
See [compiler notices](userspace/metal/NOTICE.md) and the resolved Cargo
dependency license files/manifest under `userspace/metal/licenses`.

HackyNavi's compiler adapter, C interface and authored fixtures are MIT.
LLVM and Khronos SPIRV-Tools are external compiler/validator tools, not part
of the kext. The optional Vulkan semantics test calls the installed operating
system's driver through `ash`; this is not HackyNavi GPU execution.

## Userspace AMD shader backend

`userspace/compiler` is an MIT HackyNavi adapter built against Mesa revision
`35b085c4c067f198ce0b6437d94678e47b9c645f`: NIR, ACO, AMD common helpers and
utilities. Original per-file licenses and source hashes are retained under
`userspace/compiler/licenses/mesa` and `source-manifest.json`. Staged build
compatibility changes are recorded in its `NOTICE.md`; the pinned checkout is
not patched. Cobalt64 informed boundary review; its winsys, device, validator
and frontend implementations were not copied into this adapter.

`userspace/compiler/fixtures/apple-add-one` preserves one authored MIT kernel's
recorded AIR/SPIR-V/ACO product and its exact provenance/license chain. The
native sealed class embeds only its 104-byte generated machine-code image;
Mesa and the translator are not linked into the kext. The generated header
is reproducibly verified against the pinned artifact, and no hardware
execution is implied by packaging it. See the fixture's README and manifest.

## Userspace AMD display calculations

`userspace/display/vendor/dml` contains twenty unmodified AMD DML32 source and
header files from Linux revision `3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`.
These files retain their MIT copyright and permission notices. The source
manifest records exact paths, sizes and hashes. The test-only reference IP/SoC
initializers also preserve their AMD notice. Original userspace compatibility
and adapter code is MIT licensed; it invokes the real formulas outside the
kernel. See `userspace/display/NOTICE.md` for the component boundary.

The production `userspace/display/dml32_asic.c` policy adapter and
`src/display_model_input.cpp` reference acquisition adapt the same pinned
AMD DCN3.2 sources and retain AMD's MIT notice. Their board/clock derivations,
synthetic DC-ceiling calculation policy and source links are described in
`userspace/display/PROFILE.md`. These are separate from the unchanged vendor
DML formulas and from the explicitly synthetic test observations.

## AMD binary firmware

Kernel builds embed unmodified AMD firmware from linux-firmware commit
`afabaf773c4c2e2c841429933a6a084a5af4d14d`, verified against the sizes and
SHA256s in `firmware/manifest.json`. These binary resources are separately
licensed under [AMD's firmware license](firmware/LICENSE.amdgpu), not MIT.
The build copies that complete notice into the kext's Resources directory
alongside the firmware manifest. Downloaded bytes and generated arrays are
build inputs/outputs, not tracked source files.
