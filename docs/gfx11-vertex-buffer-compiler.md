# Genuine Apple buffer vertex shader through GFX11 ACO

The isolated `gfx1100-vertex-buffer-ngg-v1` compiler accepts the actual captured
Apple vertex shader that returns `positions[vertexID]`. Apple AIR and metallib
translate to the same validated SPIR-V. Mesa NIR/ACO compiles that input to a
124-byte Navi 31 product, including 104 executable bytes. Its machine code has
four indexed buffer loads, a Position export from the loaded components, and
a separately guarded primitive export. The shader does not substitute constant
geometry or color.

This is a completed narrow **offline compiler profile**. It is not evidence of
a macOS GPU draw, an implemented general vertex compiler, a completed Metal
driver, or WindowServer acceleration. Kernel admission, owned resource binding,
complete graphics state, submission, fence completion and observed pixels need
their own evidence.

## Authoritative products and validation

- Actual Apple capture: `build/apple-vertex-buffer-capture-1/report.json`,
  copied with its artifacts to `userspace/compiler/fixtures/apple-vertex-buffer`.
  Apple toolchain 32023.864, MSL 2.4, deployment target macOS 15.0. The compiler
  host reports macOS 15.8 build 24H22; this is not a claim about the GPU target's
  installed OS. AIR SHA-256:
  `bc8be8f9da5a3644e9b159f5d40f1768b2195a14f41a0f4ad471c8f6df506027`.
  Metallib SHA-256:
  `a98e9a44782d8de99124e71ac5ff52477880005471ff2029648b3d5da9cd43f4`.
- Actual translation: `build/apple-vertex-buffer-translation-1/report.json`.
  Both real inputs pass actual SPIRV-Tools Vulkan 1.2 validation.
- Strict compiler build: `build/gfx11-vertex-buffer-compiler-2/report.json`,
  SHA-256 `0c3ed411b1576bee4ca419f24f516475c7763d0b563ffcfd8516070cf33600ca`.
  The adapter is compiled with `-Werror -Wextra`. The CLI and shared C ABI use
  the same C source and twelve existing pinned Mesa archives. Preflight and
  actual compilation bind 143 dependencies, the materialized Mesa manifest,
  generated headers, recorded build graph and compiler executable. Existing
  archives and the earlier compute executable retain their exact hashes.
  This build reuses established archive bytes and the prior build receipt;
  it does not independently reconstruct those archives from source.
- Actual tests: `build/gfx11-vertex-buffer-tests-1/report.json`, SHA-256
  `08ef7a5cf0c1d0b1a43702a8c3a8a10459c2e8d6961ff0cd83f0a54a97692bd7`.
  All 1,428 checks pass. Genuine AIR and metallib each compile through the CLI
  and shared C ABI to identical code and metadata. Twenty-two actual
  validator-accepted semantic mutations are rejected by the compiler. Tests
  reject 992 single-bit machine-code mutations, every truncated extent and
  incompatible metadata. Ninety-six equivalent register/load layouts and
  equivalent SPIR-V shift arithmetic pass, so validation is not a whole-image
  digest allowlist. Actual Mesa descriptor generation, its 48-bit boundary,
  output preservation on failure, null/invalid C ABI arguments and preliminary
  SPIR-V framing are checked. Existing compiler profiles remain unchanged.

All four genuine products have code SHA-256
`5f30f02f55c3dd5919c39672f0ebd1d233ad720efd7da9cd2ad3bdc5d46920f2`
and metadata SHA-256
`62bf08e9d42152f81e97c957398a3bcb904096e5f9ce902bb1b7a3255cc0f26f`.
The decoder verifies instruction fields and data provenance; it does not
simulate execution.

## Resource and draw contract

The reflection is deliberately preserved: `declared_size = 16`,
`extent = Unbounded`, no static ranges, and four strided accesses at byte
offsets `16 * VertexIndex + {0,4,8,12}`. Its `has_unbounded_access = false`
means the translator represented the index expression; it does not establish
a finite allocation or restrict VertexIndex. The independent kernel owner must
enforce exactly three vertices starting at zero, one instance and base vertex
zero before a 48-byte descriptor is sufficient. A userspace-provided bound,
GPU address or descriptor cannot grant that authority.

| Item | Actual profile |
| --- | --- |
| GPU / wave | Navi 31, GFX11/gfx1100, wave32 |
| Hardware stage | NGG passthrough, no allocation message |
| System SGPRs | s0..s7 reserved by the merged stage; s3 is merged wave info |
| User SGPRs | raw descriptor s8..s11; base vertex s12 |
| Vertex input | VertexID v5; packed primitive v0 is preserved |
| Indexed address | `(base_vertex s12 + VertexID v5) << 4` |
| Loads | four raw MUBUF load_dword OFFEN accesses at 0, 4, 8, 12 |
| Load completion | vmcnt(0) before Position export |
| Position / primitive | Position at byte 72; primitive at byte 92 |
| Branch destinations | no-vertex path at byte 80; no-primitive path at byte 100 |
| Buffer | nonzero 16-byte-aligned owned GPU address, 48 readable bytes |
| Raw descriptor | `[address low32, address high16, 48, 0x30016fac]` |
| RSRC1 / RSRC2 | `0x002c0002` / `0x0000000a` for the actual ACO allocation |
| Allocation | ACO reports 108 SGPRs and 24 VGPRs |
| Excluded | scratch, LDS, spills, attributes, constants, relocation, textures, extra resources |

The source validates the readonly singleton buffer interface, affine address
provenance and exact XYZW output before invoking the real Mesa NGG lowering
and ACO backend. It accepts the transient Int64 capability in genuine Apple
translation because its uint32-to-uint64-to-uint32 conversion folds away.
Residual wide arithmetic is rejected. Source arithmetic that changes position
components, resource identity, index stride or component offsets is rejected.

## C ABI and reproducibility

`hackynavi_gfx11_vertex_buffer.h` declares two functions, also the only two
named exports in the actual x86-64 Windows DLL:

```c
int hn_gfx11_vertex_buffer_compile_validated_file(
    const char *spirv_path, const char *entry,
    const char *private_empty_output_dir);
int hn_gfx11_vertex_buffer_descriptor(uint64_t address, uint32_t output[4]);
```

Both return zero on success and two on rejection/failure. Before compilation,
the caller must validate immutable SPIR-V using actual SPIRV-Tools Vulkan 1.2,
serialize calls sharing Mesa globals and supply a private empty output
directory. Any partial output must be discarded after failure. The descriptor
function calls Mesa's real raw descriptor builder; invalid or overflowing
addresses leave the output unchanged. The kernel still derives its own owned
descriptor independently. Neither function executes GPU work. A Windows C ABI
build does not establish that this new adapter is included in the Darwin
compiler bundle; that integration is separate.

```powershell
C:/Python314/python.exe userspace/compiler/build-vertex-buffer.py --output build/gfx11-vertex-buffer-compiler-N
C:/Python314/python.exe userspace/compiler/test-vertex-buffer.py --compiler build/gfx11-vertex-buffer-compiler-N/hackynavi-aco-vertex-buffer.exe --translation build/apple-vertex-buffer-translation-1 --output build/gfx11-vertex-buffer-tests-N
```

Output directories must be fresh. Evidence is retained rather than overwritten.

## Mesa provenance and licensing

The materialized Mesa source and read-only research checkout are pinned to
`35b085c4c067f198ce0b6437d94678e47b9c645f`. Relevant source locations at that
revision are:

- `src/amd/common/ac_descriptors.c:880`: real raw buffer descriptor construction.
- `src/amd/common/nir/ac_nir_lower_ngg.c:1493`: real NGG no-GS lowering;
  the no-message passthrough path is at line 1604.
- `src/amd/common/nir/ac_nir_lower_intrinsics_to_args.c:315`: actual base-vertex
  argument mapping for `load_first_vertex`.
- `src/amd/vulkan/radv_shader_args.c:801`: merged system SGPRs, followed by
  the GFX11 GS/ES VGPR ABI. The adapter places its own bounded user descriptor
  and base-vertex fields after the reserved system registers.
- `src/amd/vulkan/radv_shader.c:2336,2593`: user SGPR count and NGG RSRC fields.
- `src/amd/vulkan/radv_cmd_buffer.c:4176`: NGG stage selection and passthrough
  configuration, which are only part of a complete graphics draw state.

Mesa's original per-file licenses and notices continue to apply to the reused
libraries. The HackyNavi adapter, public header, scripts and authored shader
carry MIT identifiers. No Mesa research/source files were modified for this
completion, no upstream submission was made, and no Apple framework/private
cache implementation was copied.
