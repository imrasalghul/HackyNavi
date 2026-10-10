# Real gfx1100 position-only NGG compiler prerequisite

The separate `userspace/compiler/ngg.c` adapter compiles a validated SPIR-V
vertex entry through actual Mesa NIR NGG lowering and ACO's
`AC_HW_NEXT_GEN_GEOMETRY_SHADER`. The authored triangle fixture selects its
Position from Vulkan VertexIndex; its code is compiled from that input, rather
than supplied as a preassembled shader. This is an offline compiler milestone.
It does not submit a draw or implement a Metal render pipeline.

The accepted source profile has one full float32 vec4 Position output,
VertexIndex as its only optional input, and constant/VertexIndex selection
operations. Vertex buffers, descriptors, other varyings or system inputs,
source control flow, wide values, specialization and other arithmetic reject
explicitly. Validation by real SPIRV-Tools for Vulkan 1.2 is a documented caller
precondition. Tests validate every supported and unsupported source first.

## Actual lowering and hardware ABI

Mesa is pinned to `35b085c4c067f198ce0b6437d94678e47b9c645f`. Paths below are
relative to `.research/mesa` and are bound in the source-freeze report.

| Contract | Actual source |
| --- | --- |
| gfx11 vertex stages use NGG, not legacy hardware VS | `src/amd/common/ac_hw_stage.h:12`; `src/amd/vulkan/radv_shader.c:2190` |
| NGG NIR lowering and required options | `src/amd/vulkan/radv_shader.c:997–1073`; `src/amd/common/nir/ac_nir_lower_ngg.c:1493` |
| Passthrough preserves topology/counts; gfx11 uses late primitive export | `src/amd/vulkan/radv_shader_info.c:1399–1418` |
| Navi31's actual no-allocation-message feature | `src/amd/common/ac_gpu_info.c:348`; `ac_nir_lower_ngg.c:1604` |
| No WGP mode for NGG on gfx10.3 and later | `src/amd/vulkan/radv_shader.c:1639–1652` |
| Merged s0–s7 prefix, user SGPR0 at s8 | `src/amd/vulkan/radv_shader_args.c:608–624`, `:800–824` |
| Packed primitive in v0, VertexID in v5 | `src/amd/vulkan/radv_shader_args.c:857–878`; `src/amd/common/ac_shader_args.h:138–171` |
| Lower actual NGG/system intrinsics to arguments | `src/amd/common/nir/ac_nir_lower_intrinsics_to_args.c:269–285`, `:315`, `:436`; `src/amd/vulkan/radv_pipeline.c:498` |
| Gfx11 GS resource component counts and allocation | `src/amd/vulkan/radv_shader.c:2525–2595` |
| NGG stage selection and wave32/passthrough bits | `src/amd/vulkan/radv_cmd_buffer.c:4151–4200` |
| SOP, VOP and export encodings | `src/amd/compiler/aco_assembler.cpp:151–238`, `:347–398`, `:479–504`, `:988`, `:1074–1139`; `aco_opcodes.py` |

The current product uses wave32, CU mode, a maximum NGG workgroup of 256,
triangle topology and passthrough with `PRIMGEN_PASSTHRU_NO_MSG`. It disables
culling, streamout and primitive-ID outputs. With no parameter exports, actual
Mesa lowering requires no shader LDS and writes no attribute ring; both are
checked again in the final ACO callback. That does not settle whether later
draw-state setup must allocate/configure an attribute ring for other hardware
requirements or workarounds.

The owned argument table reserves the hardware system prefix and all five GS
VGPR positions. It puts the one real user value, base vertex, in s8; packed
primitive data comes from v0 and VertexID from v5. Actual merged-wave vertex
and primitive counts come from s3. Unused address/system slots are reserved
without being read or dereferenced. ACO's final product rejects scratch, LDS,
spills, shared VGPRs, constants storage, relocation records and memory/resource
intrinsics outside this profile.

`shader.json` records actual ACO allocation/float mode, GS RSRC1/2, the required
stage-selection value, Position format and explicit argument identities. These
are partial shader/draw requirements, not a complete safe graphics state. In
particular, subgroup topology/count state, CU masks, residency, addresses,
render-target layout and native retirement remain separate work.

## Local evidence

`build/gfx11-ngg-local-3/report.json` records the successful strict C adapter
compile and real link against the twelve existing Mesa archives. The builder
uses the reviewed fragment build's provenance method in a separate new file:
actual preflight/compile dependency lists agree, 63 materialized Mesa headers
match the prior source manifest, and eight generated headers, 69 toolchain
headers and two adapter inputs are separately hash-bound before and after.
The prior build report and its manifest identity are retained. This proves the
current compilation's inputs and exact reused archive bytes; it does not
reconstruct the archives' full original build. Existing compute and fragment
sources, binaries and immutable reports were not changed.

The baseline triangle produces 148 code bytes, with 128 executable bytes and
five ACO padding words. ACO reports 108 SGPRs, 24 VGPRs and no scratch/LDS. The
actual code reads the merged-wave counts, establishes the vertex execution
mask, adds base vertex to VertexID, selects positions, exports POS0, waits for
that export, establishes the primitive mask, exports the original packed v0
primitive, and ends. GS RSRC1 is 2,883,586 and RSRC2 is 2.

The existing ACO build has no LLVM AMDGPU disassembler, so its labelled
print-program fallback remains `aco-diagnostics.txt`. `verify-ngg.py` decodes
the actual machine words for this small SOP/VOP/EXP profile. It checks literal
and instruction extents, defined source registers, allocation bounds,
unmodified packed primitive v0, exact export channels/DONE bits, forward
export-guard targets, the intervening export wait, termination and padding.
Its listing is derived from the binary, not parsed from ACO's text. It does
not execute instructions, prove numerical positions at runtime, replace GPU
execution or serve as a kernel admission boundary.

`build/gfx11-ngg-tests-3/report.json` records 234 checks:

- Four real products cover the authored triangle and changed inline, literal
  and signed Position constants. Source changes alter actual machine code.
- Eight validated unsupported cases reject a non-Position output, vertex
  attributes, InstanceIndex, missing Position store, specialization, dynamic
  integer-to-float conversion, a missing entry and the actual SDL vertex.
- 169 malformed product cases cover every baseline truncation, export
  channels/DONE/source, primitive corruption, missing export wait, invalid
  branches/end/padding and metadata mismatches.

The real owned SDL metallib compiler translated `SDL_Solid_vertex` into valid
SPIR-V. Its actual reflection includes vertex attributes and two uniform
buffers, so this narrower NGG adapter rejects it with
`resources and vertex attribute inputs are unsupported`. It does not discard
those inputs or replace that shader with the authored triangle.

```powershell
C:/Python314/python.exe userspace/compiler/build-ngg.py --output build/gfx11-ngg-new
C:/Python314/python.exe userspace/compiler/test-ngg.py --compiler build/gfx11-ngg-new/hackynavi-aco-ngg.exe --output build/gfx11-ngg-tests-new
```

Native compilation, private graphics product admission, owned render-target
state, actual draw submission and pixel/guard readback remain unverified or
unimplemented as described in the fragment prerequisite document. No mini
job, Metal object construction or GPU operation was performed for this slice.
