# GFX1100 instruction research artifact

`gfx1100-leaf-decoder-rows.json` is generated from five hash-checked Mesa files
and four genuine ACO products retained in
`../fixtures/compute-products/products.json`. It contains18 observed opcode
rows, physical operand fields, source operand types where Mesa supplies them,
explicit forbidden examples, and every decoded instruction/literal/padding
word of the four products. The table is research input for a future native
validator. Every row has `admission=false`.

Run from the repository root:

```text
python userspace/compiler/generate-gfx11-decoder-rows.py --check
python userspace/compiler/test-decoder-rows.py
```

Without `--check`, the generator writes the artifact. It needs only the existing
pinned `.research/mesa/src/amd/compiler` sources, not a compiler build. Source
hashes normalize CRLF to LF before checking the pinned content. Unrecognized
opcodes/formats, unreviewed modifiers/special registers, truncated instructions
or literals, and invalid executable/padding boundaries fail.

To reproduce the real inputs with the existing compiler and owned-pipeline
library, set `METAL2VULKAN_LLVM_DIS` and `METAL2VULKAN_SPIRV_VAL` to their actual
tools and run:

```text
python userspace/compiler/capture-decoder-products.py --library PATH_TO_PIPELINE_LIBRARY --compiler PATH_TO_HACKYNAVI_ACO
```

The capture compiles the actual Apple AIR `add_one` fixture and authored
`affine_xor.ll`, constructs4099-thread plans at nominal local64, copies the
owned local64/local3 code after freeing the source shader, and compares each
variant with a separate genuine ACO invocation. Temporary compiler directories
are removed. No GPU is accessed. Tool binary hashes and original source/SPIR-V
hashes accompany the retained bytes. Recapturing with another library binary
can change provenance hashes without changing code; regenerate and review the
artifact after an intentional recapture.

The currently available Windows LLVM20 binary has no AMDGPU target. No
independent LLVM disassembly success is claimed. The generator uses exact
Mesa assembler encodings and opcode rows. Tests distinguish literal data from
instruction words and exercise unknown/forbidden forms. They deliberately
show that a structurally valid uninitialized descriptor quartet is accepted
by this inspector: **provenance, initialized-register and asynchronous-load
completion analyses have not been implemented here**. These outputs must not
be connected to native execution as an admission check. See
`../../../docs/compute-product-admission.md` for the required boundary.

Opcode/encoding metadata derives from Mesa commit
`35b085c4c067f198ce0b6437d94678e47b9c645f`, particularly `aco_opcodes.py`,
`aco_opcodes_cpp.py` and `aco_assembler.cpp`, copyright2018 Valve Corporation,
SPDX MIT. Original license texts and the Mesa notice are retained in
`../licenses/mesa` and `../NOTICE.md`. The capture/generation/test adapters are
MIT, copyright2026 HackyNavi contributors. No Mesa or Cobalt native driver is
imported by this artifact.
