# Uniform compute product admission

The real ACO products for a `1x1x1` workgroup use a different instruction mix
from the existing 64-thread and three-thread products. The native verifier
now handles that mix without substituting shader code or trusting a compiler
identifier. The two added fixtures are independently identical to standalone
ACO output captured in `build/compute-prototype-isa`:

| Function | Executable / supplied bytes | SHA-256 of supplied code |
|---|---|---|
| `add_one` | 84 / 104 | `ce6d630314a3aeeb90343813ad70146644851a7ca13532dd5fe0247e0d46cb69` |
| `affine_xor` | 100 / 120 | `6c1aec25b2e75152f0bacf0171fee370420c432e19b5ce344d4d8a9064b913b5` |

Both declare wave32, 108 SGPRs, 24 VGPRs and derived RSRC values
`0x202c0002`, `0x788`, `0`. The captured `aco.txt` is an intermediate compiler
listing; `shader.bin` and `code-words.txt` contain the final instruction stream.
The initial probe rejected `s_add_u32` at byte 36. No GPU execution is implied.

## Implemented instructions and address form

Six additional instructions have narrow, explicit handling:

- `s_add_u32`, `s_lshl_b32` and `s_xor_b32` use the GFX11 SOP2 opcodes
  0, 8 and 26. They propagate constants and provable trailing zero bits. SCC
  writes are implicit, but no admitted instruction reads SCC or changes EXEC.
- `v_mov_b32` uses VOP1 opcode 1 and checks the actual source and destination.
- `v_readfirstlane_b32` uses VOP1 opcode 2, requires a VGPR source and ordinary
  SGPR destination, and waits for the source's memory result. The direct
  nonempty launch and unchanged EXEC guarantee an initialized active lane.
  The result is data; it cannot create a root or descriptor tag.
- `v_and_b32` uses VOP2 opcode 27 to unpack local-X from the packed local
  invocation ID for multidimensional workgroups. Its alignment proof takes
  the larger trailing-zero count of its operands; either operand can clear a bit.

Encodings are checked against pinned Mesa
`35b085c4c067f198ce0b6437d94678e47b9c645f`,
`src/amd/compiler/aco_opcodes.py` and `aco_assembler.cpp` (SOP2 and VOP1
emitters). Special registers, literal extension selectors and unsupported
opcodes remain rejected. These additional forms are not in the historical
four-product structural table in `userspace/compiler/generated`; that table
is a research artifact, not the production verifier.

The new products still use **vector buffer memory instructions**. No scalar
buffer memory instruction has been added. Their uniform address form has
`OFFEN=0`, `IDXEN=0`, an unused VADDR field of 128, a zero immediate offset and
a provably dword-aligned SOFFSET. The prior `OFFEN=1` form continues to require
a zero SOFFSET and an aligned vector offset. Cache/LDS/TFE flags remain excluded.

The native descriptor builder supplies an immutable descriptor with stride
zero, byte length and raw bounds mode 3. That mode includes the scalar offset
and the transferred payload in its range check; it does not treat SOFFSET as
permission to move outside the buffer. This is specified in the
[AMD RDNA3 ISA, section 9.4.1 and Table 46](https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/instruction-set-architectures/rdna3-shader-instruction-set-architecture-feb-2023_0.pdf)
and corresponds to pinned Mesa `src/amd/common/ac_descriptors.c`'s raw-buffer
descriptor construction. Actual descriptor publication is still performed by
`gfx11_dispatch_product.cpp`, after native resource ownership checks.

The existing eight-byte scalar state keeps either a tuple component or a
data alignment proof in its last byte, distinguished by its kind. Breaking a
root/descriptor tuple clears that byte on every component. Copies, scalar
arithmetic and lane transfers cannot copy or manufacture provenance.

## Verification and remaining work

`build/gfx11-uniform-product-local-2/report.json` records 99,961 strict checks
and 99,961 UBSan checks, zero failures. Coverage includes seven actual ACO
fixtures, every VOP1 source/destination selector, excluded VOP1 opcodes,
alignment propagation, pending loads, tuple destruction, clauses, launch
limits, truncations and mutated instructions. The first run exposed one
outdated test expectation: the uniform affine product uses two VGPRs rather
than the previous vector product's three; the expectation was corrected.

Independent review passed; its report is stored alongside the local result.
`build/native-gfx11-uniform-admission/report.json` records 99,961 native
ASan/UBSan admission checks, 120,732 direct dispatch checks and a successful
real-header kernel-object compile with a 2 KiB frame limit. The nine-file
snapshot was verified and the 1,076 KiB stage removed. Kernel object SHA-256:
`92d43f3aa6ce39b8b818e56100172c01f2c77011d89e739d5de067d52a8d3115`.

The focused actual compiler boundary also passes 265 strict and 265 UBSan
checks over nine cases and twenty variants, including the fresh Apple
macOS15 AIR/metallib, both one-thread prototypes, full/tail pairs and all eight
multidimensional regions. See `build/compute-pipeline-admission-local-2`.
The prior failure from unpacking the packed local ID remains preserved.

The last full kext and issued diagnostic package predate this change.
Passing these CPU checks does not establish target GPU execution or Metal
device discovery.
