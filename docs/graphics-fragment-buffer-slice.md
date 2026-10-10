# Actual fragment buffer compiler and next native boundary

The additive offline compiler now translates a genuine Apple fragment shader
that reads one read-only color buffer into actual gfx1100 PS code. The existing
resource-free fragment.c, compute compiler, NGG compiler and graphics driver
profile were not expanded. No GPU execution or buffer-backed draw is claimed.

## Actual source and provenance

`userspace/compiler/fixtures/apple-fragment-buffer/fragment-buffer.metal` defines
`hn_buffer_fragment(constant float4& color [[buffer(0)]]) { return color; }`.
Installed Apple Metal 32023.864 produced the retained AIR/metallib with
macOS 15/MSL 2.4 flags on macOS 15.8/24H22, not the target 15.8.1. The actual capture
report verifies all exported hashes and removal of its 7,123-byte stage.
No tools were installed and no Metal objects were constructed.

The real explicit-tools shader API produces identical SPIR-V from both formats.
Reflection reports one read-only Metal0/set0/binding0 buffer, declared 16 bytes
and static accessed range `[0,16)`. The actual program loads four scalar u32
words at offsets0/4/8/12, bitcasts them to float, and stores RGBA. No source,
reflection or machine fixture substitutes for these reads.

The new fragment-buffer.c uses pinned Mesa
`35b085c4c067f198ce0b6437d94678e47b9c645f`. Before actual PS export lowering and
ACO, it proves one unconditional NIR block, four disjoint readonly aligned
scalar 32 loads covering exactly 16 bytes, and bit-preserving RGBA provenance.
It rejects other resources, varyings/system inputs, descriptor indexing,
stores, volatile/coherent loads, dynamic offsets, arithmetic, reordered output
components and unsupported capabilities. Only the proven descriptor reference
is bound to a real AMD four-SGPR argument.

The strict builder binds all 12 reused archive hashes, prior actual build-report
and source-manifest identities, and actual compiler dependencies before/after
compilation. Materialized Mesa headers must match that manifest; generated and
toolchain headers are classified separately. This proves reuse of exact archive
bytes; their historical source reconstruction is not independently repeated.

## Actual fixed PS and descriptor contract

The product has 52 executable bytes plus five code-end DWORDs:72 bytes total.
Actual allocation is 108 SGPRs / 24 VGPRs, wave32/CU, float mode 192, no scratch,
LDS, spills or relocations. No scalar register writes occur.

| State | Actual value |
| --- | --- |
| User SGPRs | Four native-owned descriptor words in s0..s3 |
| PS RSRC1 / RSRC2 | `0x002c0002` / `0x00000008` |
| PS INPUT_ENA / INPUT_ADDR | `0x80` / `0x80`; LINE_STIPPLE value unused |
| PS IN_CONTROL | `0x8000` (wave32) |
| COL_FORMAT / CB_SHADER_MASK / Z_FORMAT | 9 / 15 / 0 |
| Descriptor extent | Nonzero 16-byte-aligned 48-bit base, exactly 16 bytes |
| Descriptor words | `[VA_low32, VA_high16, 16, 0x30016fac]` |
| Memory | Four buffer_load_dword, immediate offsets0/4/8/12 |
| Address operands | s0..s3, inline-zero SOFFSET, OFFEN=IDXEN=TFE=0, unused VADDR128 |
| Readiness | Actual VM0 wait before any export reads loaded VGPRs |
| Export | Full uncompressed MRT0 RGBA, DONE, no ROW_EN |

Decoded actual bytes:

```text
bf850003                         s_clause 3
e0500000 80000080                 buffer_load_dword v0, s[0:3], 0 offset:0
e0500004 80000180                 buffer_load_dword v1, s[0:3], 0 offset:4
e0500008 80000280                 buffer_load_dword v2, s[0:3], 0 offset:8
e050000c 80000380                 buffer_load_dword v3, s[0:3], 0 offset:12
bf8903f7                         s_waitcnt vmcnt(0)
f800080f 03020100                 exp mrt0 v0, v1, v2, v3 done
bfb00000                         s_endpgm
bf9f0000 x5                      ACO code-end padding
```

The additive `hn_gfx11_fragment_buffer_descriptor` calls actual Mesa
`ac_build_raw_buffer_descriptor(GFX11, false, VA, 16, out)`. Invalid address or
null output returns2 without modifying output. Five real descriptor results,
including the last aligned address ending at 48-bit limit, match the table.
This is an offline reference, not a kernel API accepting caller addresses.

Pinned primary sources:

- `.research/mesa/src/amd/compiler/instruction_selection/aco_select_nir_intrinsics.cpp`
  load_buffer (1473 onward), visit_load_ssbo (2155 onward): actual MUBUF selection.
  No SMEM override was imposed; the real output uses VMEM.
- `aco_assembler.cpp:554-603`: GFX11 MUBUF fields; `aco_opcodes.py:1714`:
  buffer_load_dword opcode0x14. `aco_assembler.cpp:988`: GFX11 EXP/DONE/ROW_EN.
- `aco_form_hard_clauses.cpp:37-83`: count-minus-one and VMEM-load clause class.
- `aco_ir.cpp:1452-1459`: VM bits15:10, LGKM9:4, EXP2:0; 0x03f7 means
  VM0/LGKM63/EXP7. The VM wait cannot be replaced by a scalar-only wait.
- `.research/mesa/src/amd/common/ac_descriptors.c:880`: raw descriptor builder;
  its GFX11 OOB contract includes payload size against NUM_RECORDS. STRIDE0,
  OOB_SELECT RAW3, exact16-byte bounds.
- `.research/mesa/src/amd/vulkan/radv_shader.c:2336,2366,2485-2488`: real
  user-SGPR, VGPR, float mode and PS memory-order resource fields.

Compiler source locations above without full paths are in
`.research/mesa/src/amd/compiler/` at the stated pinned revision.

## Offline validation and its limits

`verify-fragment-buffer.py` independently decodes the bytes and tracks immutable
s0..s3 descriptor provenance, exact clause extent, distinct resource offsets,
allocated destinations, pending VMEM readiness and component provenance at
export. No extra memory, scalar writes, arithmetic or control flow is accepted.
It is not kernel admission and does not establish real residency, permissions,
cache visibility or GPU completion.

`build/gfx11-fragment-buffer-tests-4/report.json` records 873 checks: actual
AIR/metallib compilation equivalence, 15 SPIRV-Tools-valid rejected shader
variants, all 576 single-bit machine mutations, every shorter extent, metadata
failures, 96 valid register/order permutations, actual Mesa descriptor boundaries,
and mutual rejection between unchanged constant and new buffer profiles.
The permutations prove this is not a whole-image hash allowlist.
ACO's text diagnostics explicitly identify an IR fallback; the separate decoder
performs the machine-byte check instead of calling that fallback disassembly.

Evidence chain:

- `build/apple-fragment-buffer-capture-1/report.json` and fixture provenance.
- `build/apple-fragment-buffer-translation-1/report.json`.
- `build/gfx11-fragment-buffer-compiler-4/report.json` and dependency snapshots.
- `build/gfx11-fragment-buffer-tests-4/report.json` and actual products/listings.
- `build/gfx11-fragment-buffer-local-1/source-freeze.json`: final new sources,
  fixture, docs, evidence and pinned encoding references.

## Next native boundary (separate work)

Root owns kernel/profile, BO, wire and Metal integration. Native code must copy
real stage bytes and independently admit this memory-reading profile; compiler
JSON must not authorize addresses, raw resource state or ISA. The descriptor
tuple is immutable native provenance; loaded results stay pending until the
actual VM0 wait, and RGBA must preserve offsets0/4/8/12. Derive PS state from
admission while preserving the existing resource-free profile.

Generate the descriptor from one live same-owner BO handle/generation and
mapping with the exact16-byte aligned range. Reject code/target/control aliases
and overflow. Page rounding must not widen the descriptor. Retain source BO and
VM/descriptor identity through actual fence, consumption and recovery. Copied
public requests carry handles/ranges, never descriptor words or GPU addresses.

The runtime/Metal command records must retain the real buffer+offset, require
the same device/context and implement only supported index 0. Managed uploads
use real synchronization. CPU reading the color cannot replace shader access.
A later public target test should use the same real BO for two uploads/draws,
then consume a compute-written color directly without CPU readback/reupload.
Neither target test ran in this offline compiler milestone.
