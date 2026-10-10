# Portable graphics shader admission

`gfx11_graphics_product` implements the narrow
`Gfx1100PositionConstantColorV1` **kernel-side shader admission** contract.
It validates actual instruction bytes and derives the limited shader state.
It does not accept compiler JSON, caller-provided hardware register values,
addresses, resource masks, raw PM4, executable lengths or export declarations.
The existing offline Python checkers remain diagnostic tools, not this boundary.

This does not yet submit a draw. Native code-image ownership, code/target BO
bindings, complete NGG and raster state, actual drawing and GPU pixel readback
are subsequent work. A successful `Report` is diagnostic data, never ownership
or permission to submit. No target GPU operation was performed for this change.

## Owned image contract

The caller first copies the complete image into its own serialized, readable
storage. NGG code starts at offset zero; PS code starts at the next 256-byte
boundary after NGG; the image ends at the next 4096-byte boundary after PS.
Only the two code lengths are inputs. Each stage is at most 1024 bytes,
including exactly five real ACO `s_code_end` words. The validator derives the
executable lengths by subtracting those 20 bytes. All interstage and final
allocation padding must be zero. For this profile the canonical image is one
4096-byte page, containing two independently aligned entry points.

Pointer-wrap, lengths and output aliasing are checked before reading code.
Failures clear derived output. An output overlapping the claimed image is
instead rejected without changing output or input; otherwise writing the
failure report would corrupt the bytes we promise to preserve. This rule is
explicit in the header and tested.

The eventual native caller must resolve real BO handles, generations, owner,
VMID and mappings, establish private RX residency without writable aliases,
revalidate that owned copy, and retain unchanged bytes through actual GPU
completion or recovery. A numeric offset and a portable `Report` establish
none of those properties by themselves.

## Control and register proof

The NGG profile admits the actual ACO wave32 passthrough control skeleton:

1. A fixed prefix derives the vertex mask and primitive mask from hardware
   merged-wave information in s3, retains the primitive mask in s0, and writes
   the vertex mask from s2 to EXEC. The vertex EXECZ branch must target the
   exact export-wait join immediately after POS0.
2. The body is a bounded straight-line region. Its only initial ordinary
   values are trusted base vertex in s8 and VertexID in v5. Other hardware
   system slots are not treated as initialized data. Packed primitive v0 is
   neither readable nor writable in this region.
3. Only full-width MOV, ADD_U32, EQ/LG_I32 comparisons, CNDMASK and the reviewed
   MOV/CNDMASK VOPD pairs are admitted. Comparisons may write only s1 and
   VCC_LO; these are tracked as lane masks, not ordinary scalar values.
   Neither can alias the preserved s0/s2/s3/s8 or EXEC. Every mask use follows
   an admitted defining comparison in the vertex region. Arbitrary SALU,
   EXEC writes, branches, memory, synchronization, messages and exports are
   not body instructions.
4. One exact POS0 RGBA/DONE export consumes only proven defined VGPRs. The
   fixed tail waits `expcnt(0)`, restores primitive EXEC from s0, skips PRIM
   when that EXEC is empty, exports original packed v0 with the exact PRIM
   channel/DONE encoding, and ends. No body definitions are used at the join
   or by primitive lanes. Thus there is no lexical-only register proof across
   a conditional path: the only skip edge bypasses the entire vertex region,
   and the other region has its own independent hardware input.

The future draw owner must provide the real stage ABI: hardware-generated
merged-wave counts/masks and packed primitive/VertexID data, with trusted
draw topology/count setup. This module does not synthesize those inputs.

PS starts with **no proven data registers**, despite the hardware's required
dummy LINE_STIPPLE_TEX input. Only constant/defined-value MOV and dual MOV
instructions are admitted before one exact MRT0 RGBA/DONE export and END.
The validator computes the four exported constant bit patterns from the
actual code; it does not compare against a caller's declared color. This is
constant propagation for admission, not a CPU renderer or GPU success result.

Both halves of a VOPD read before either destination is published. Gfx11
source-port parity restrictions are enforced, as are unused operand fields,
literal extents and implicit VCC restrictions. VOP3 unused operands and all
unreviewed modifiers are rejected. A literal cannot consume bytes from an
export or control suffix. Valid ACO delay hints are permitted in NGG with
reserved fields rejected; the pinned delay pass explicitly explains that
omitting a hint affects scheduling rather than result correctness.

## Derived state and scope

The profile fixes wave32, CU mode, zero shader LDS/scratch/shared VGPRs,
zero memory/resources/parameter exports and the current floating-point mode
192. No general shader-feature or rendering capability is inferred. The
validator proves all ordinary accesses fit in v0–v23 and allocates 24 VGPRs,
the Navi31 wave32 allocation quantum. It reports the actual highest required
VGPR separately. Gfx11 hardware SGPR allocation is fixed at 108. These facts
derive RSRC1 `0x002c0002` for both stages; NGG RSRC2 is USER_SGPR(1), or 2,
and PS RSRC2 is zero.

The derived stage selection is 104931344, Position format is 4, PS input
ENA/ADDR are 128, PS W32 control is 32768, PS color export format is 9,
MRT0 component mask is 15 and shader Z export format is zero. These are
shader requirements, **not a complete draw register image**. In particular,
CU topology/RSRC3, NGG subgroup and attribute-ring requirements, render-target
layout, rasterization, viewport/scissor, barriers and retirement are absent.
No raw values for these missing components are accepted here.

## Pinned source evidence

Mesa commit `35b085c4c067f198ce0b6437d94678e47b9c645f` is available in
`.research/mesa`; this implementation links no Mesa code.

| Contract | Pinned source |
| --- | --- |
| Actual NGG lowering and exports | `src/amd/common/nir/ac_nir_lower_ngg.c:1493`, `:1604`; `src/amd/vulkan/radv_shader.c:997–1073` |
| Merged system SGPR/user layout and VertexID v5 | `src/amd/vulkan/radv_shader_args.c:608–624`, `:800–824`, `:857–878` |
| Late primitive export and passthrough | `src/amd/vulkan/radv_shader_info.c:1399–1418`; `src/amd/common/ac_gpu_info.c:348` |
| CU mode for gfx11 NGG | `src/amd/vulkan/radv_shader.c:1639–1652` |
| GS resource/component counts | `src/amd/vulkan/radv_shader.c:2525–2595` |
| Stage selection | `src/amd/vulkan/radv_cmd_buffer.c:4151–4200` |
| PS required dummy input and W32 | `src/amd/vulkan/radv_shader.c:3997–4022`, `:2056`; `src/amd/vulkan/radv_shader_info.c:1245–1250` |
| SOP/VOP/EXP instruction fields | `src/amd/compiler/aco_assembler.cpp:151–238`, `:347–398`, `:479–504`, `:988–1015`, `:1074–1139`; `aco_opcodes.py` |
| Gfx11 VOPD SGPR/literal and source-port parity/interlock | `src/amd/compiler/aco_scheduler_ilp.cpp:295–377` |
| Delay-hint semantics and encoding | `src/amd/compiler/aco_insert_delay_alu.cpp:17–28`, `:263–287`, `:365–387`; `aco_builder_h.py:129–143` |
| Navi31 allocation and fixed SGPR derivation | Existing source-bound analysis in `docs/gfx11-compute-occupancy.md`; only its hardware allocation facts are used, not its WGP compute launch formula |

## Verification

The tests embed unchanged real machine products with original path and SHA256
comments. They cover five NGG products and eight PS products in all 40 pairs,
including both actual Apple-authored entry points captured under MSL2.4 for
macOS15. Their AIR/metallib provenance remains in
`userspace/compiler/fixtures/apple-graphics/provenance.json` and the immutable
`build/apple-graphics-integration-1` report. The Apple pair has 144/56 code
bytes and 23/4 actual executable instructions; its expected output metadata
is compared against the prior real compiler products.

Additional tests cover every bit of the fixed control/export words, every
padding byte, source/opcode/register domains, reserved fields, truncated
literals, resource instructions, primitive corruption, skipped definitions,
VOPD paired-use/parity errors, output aliasing, maximum stage extents,
thousands of independently varied constant bit patterns and deterministic
byte mutations. Positive body/register/constant variants ensure this is an
instruction policy rather than a fixture-hash allowlist. Host tests establish
admission behavior only; kernel-header compilation establishes compile/frame
compatibility only. Neither executes these shaders on the GPU.
