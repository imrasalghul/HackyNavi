# Real gfx1100 fragment compiler prerequisite

The new separate adapter compiles an actual validated SPIR-V fragment entry to
gfx1100 pixel-shader machine code through the pinned Mesa NIR/ACO implementation.
Its first admitted profile is one unconditional constant float32 RGBA output at
location/index zero. It does not submit a draw, create a Metal render pipeline,
replace the compute adapter or provide a CPU rendering backend.

`userspace/compiler/fragment.c` uses `MESA_SHADER_FRAGMENT`, actual NIR output
lowering and `ac_nir_lower_ps_late`, then `AC_HW_PIXEL_SHADER`. It verifies the
constant-output contract before export lowering and the exact MRT0 export
contract afterward. The machine words come from ACO; they are not an embedded
shader fixture. Changing the source color changes the generated code.

The exact source pin is Mesa `35b085c4c067f198ce0b6437d94678e47b9c645f`.
Relevant paths below are relative to `.research/mesa`:

| Required distinction | Source |
| --- | --- |
| PS is a separate hardware stage; gfx11 pre-raster shaders use NGG | `src/amd/common/ac_hw_stage.h:12`; `src/amd/vulkan/radv_shader.c:2190` rejects legacy VS for gfx11 |
| Actual fragment output lowering and scalar input lowering | `src/amd/vulkan/radv_pipeline.c:439`; `src/amd/common/nir/ac_nir_lower_ps_late.c:335` and `:736` |
| Even a no-varying gfx11 PS needs a real enabled input group | `src/amd/vulkan/radv_shader.c:3997`; it chooses LINE_STIPPLE_TEX, including the explicit Navi31 test note |
| Register allocation and PS resource registers | `src/amd/vulkan/radv_shader.c:2316`, `:2366`, `:2485` |
| PS wave32 is a context register, not the compute dispatch bit | `src/amd/vulkan/radv_shader.c:2056`; `SPI_PS_IN_CONTROL.PS_W32_EN` |
| Fragment scheduling uses one wave, not compute workgroups | `src/amd/vulkan/radv_shader_info.c:1245–1250`; FS `workgroup_size = wave_size` |
| VOP1, VOPD and gfx11 EXP machine encodings | `src/amd/compiler/aco_assembler.cpp:363`, `:479`, `:988`; opcodes in `aco_opcodes.py:766`, `:796`, `:1024`, `:1495` |
| Actual inline constant encodings | `src/amd/compiler/aco_ir.h:604` |

The PS ABI records actual ACO allocations and float mode, PS RSRC1/2, input
ENA/ADDR, the 32_ABGR color export format, color mask and PS wave32 bit. It uses
zero user SGPRs and one initialized system VGPR for LINE_STIPPLE_TEX. The constant
shader never reads that system value. It rejects scratch, LDS, spills, constants
storage and relocations. PS RSRC3/4 topology/CU settings, render-target layout,
draw state and retirement are deliberately not represented as complete.

The baseline white shader has 36 code bytes, 16 executable bytes, 108 allocated
SGPRs, 24 allocated VGPRs and one initialized input VGPR. Its actual instructions
are `v_mov_b32`, `exp mrt0 ... done`, and `s_endpgm`, followed by five real ACO
`s_code_end` padding words. ENA/ADDR are 128, PS RSRC1 is 2,883,586, RSRC2 is zero,
color export format is 9, and `SPI_PS_IN_CONTROL` is 32,768.

The existing ACO build has no LLVM AMDGPU disassembler. Its `record_asm` output
explicitly falls back to printing the final ACO program, which is retained as
`aco-diagnostics.txt` and is not labelled machine disassembly. The new
`verify-fragment.py` instead decodes the actual emitted words for this small
MOV/VOPD-MOV/EXP/END profile, checks source register definition and allocation,
literal extents, exact MRT0/full-channel/DONE bits, termination and padding,
and proves the exported constant bit patterns match the input's NIR values.
Unknown instructions fail that proof. This is an offline fixture verifier,
not a kernel admission boundary or a shader execution fallback.

## Local validation and immutable builds

`build-fragment.py` reads the established Mesa compiler command and links the
same twelve existing real static dependency archives into a separate output
directory. It writes no existing source stage, library, build graph, compute
executable or dylib. It hashes the actual reused archives, command records,
new sources and resulting executable, and confirms those inputs plus the old
compute executable remain unchanged. An actual compiler dependency scan and
the compilation's dependency file must agree: 63 materialized Mesa headers
match the prior source manifest, eight generated headers and 69 toolchain
headers are separately hash-bound, alongside the two adapter inputs. The prior
build report is copied and its source-manifest identity verified. This binds
the current adapter's inputs and exact reused archive bytes; it does not
reconstruct or re-prove the archives' complete original source build.
No Meson graph change is needed for this
isolated prerequisite. Native compilation is a separate future validation.

```powershell
C:/Python314/python.exe userspace/compiler/build-fragment.py --output build/gfx11-fragment-new
C:/Python314/python.exe userspace/compiler/test-fragment.py --compiler build/gfx11-fragment-new/hackynavi-aco-fragment.exe --output build/gfx11-fragment-tests-new
```

The SPIRV-Tools assembler was built from the existing local source/dependency
build. Tests run the actual assembler and Vulkan1.2 validator before ACO; a
deliberately invalid module first confirms that validation rejects malformed
input. The separate C API documents the caller's validation obligation and
requires a private empty output directory whose contents are discarded on
failure. It does not change the compute ABI.

`build/gfx11-fragment-local-6/report.json` records the successful strict local
adapter compile/link. `build/gfx11-fragment-tests-4/report.json` records 133 checks:

- Seven real products cover inline constants, literal constants, distinct
  channels, negative constants, VOPD paired moves and zero.
- Eight validated-SPIR-V unsupported cases reject the wrong output location or
  type, no output, early tests, specialization, discard, wrong entry and the
  actual pinned SDL fragment.
- Fifty-one machine-product rejection checks cover every baseline truncation,
  undefined registers, missing DONE/alpha, wrong MRT, gfx11 ROW_EN, program end,
  padding and inconsistent metadata.

The real owned SDL metallib/AIR compiler produced valid fragment SPIR-V for
`SDL_Solid_fragment`; the new backend then rejected its actual interpolated
input with `varying/system inputs are unsupported`. It neither erased the
varying nor returned a replacement constant shader. Existing translator and
compute compiler binaries were not rebuilt. No GPU operation or Metal object
construction occurred.

The final adapter uses `workgroup_size = 32` for ACO scheduling, matching
RADV's fragment-stage rule that this value equals the wave size. It is not a
compute dispatch group. `build/gfx11-fragment-tests-4/provenance-checks.json`
records nine dependency-provenance checks, including mismatch rejection, and
confirms all seven code/metadata outputs are byte-identical to the earlier
products made before that scheduling correction. Earlier reports are retained
as historical evidence; the final source freeze supersedes them.

## What the current driver still lacks for a draw

The current `gfxqueue` implementation owns a real legacy graphics ring, an
immutable indirect-buffer path, VMID ownership and GPU fence observation. Its
admitted products are compute, and `src/gfx_queue.cpp:218` currently adds only
CS_PARTIAL_FLUSH/PFP synchronization to the isolated submission tail. That
tail alone is not a pixel-shader/color-buffer retirement contract.

A real offscreen draw still needs these connected pieces:

1. A vertex/primitive program using gfx11 NGG lowering and its actual argument,
   primitive export, LDS and attribute-memory contract. RADV supplies
   `ac_nir_lower_ngg_nogs` with real stage/output information at
   `radv_shader.c:1042–1073`; a legacy VS enum is not an alternative on Navi31.
2. Owned resident shader and target allocations, correct color-target format,
   surface layout and bounds, actual NGG attribute storage when required,
   viewport/scissor/raster state and exact stage register bindings. The pinned
   libdrm reference at `.research/libdrm/tests/amdgpu/shader_test_util.c:1105`,
   `:1254`, `:1440` and `:1685` contains a real gfx11 offscreen draw sequence.
   Its fixed topology/state constants and large attribute allocation are
   evidence to audit, not safe values to copy without their ownership contract.
3. A private validated graphics product and PM4 encoder, including actual
   DRAW_INDEX_AUTO and fragment/color-cache completion. Existing native
   compute admission must not accept arbitrary graphics PM4. The Linux/libdrm
   test's completion also relies on amdgpu kernel submission/fence machinery;
   copying its userspace IB alone does not reproduce that machinery.
4. A target execution/readback test that verifies actual pixels and guard
   regions before exposing a Metal render encoder, drawable or display path.

The frozen verified 14-object compute frontend remains unchanged. This new
compiler prerequisite makes no system-discovery, rendering, display, HDR or
target-GPU success claim.
