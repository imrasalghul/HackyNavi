# Actual Apple graphics shader fixture

`triangle.metal` is authored HackyNavi source under the MIT license. It has a
resource-free VertexID triangle with Position output, and a fragment returning
RGBA `(0.25, 0.5, 0.75, 1.0)`. `graphics.air` and `graphics.metallib` are the
actual outputs of the installed Apple Metal compiler, with no replacement
shader or edited AIR. `provenance.json` records the exact commands, source and
artifact hashes, native toolchain version, export verification and cleanup.

The capture used the existing toolchain
`com.apple.dt.toolchain.Metal.32023.864` with `-std=macos-metal2.4` and
`-mmacosx-version-min=15.0`. It staged 10,764 bytes on the Mac and removed that
unique stage after all three files were exported and hash-verified. No tool
installation, Metal object construction, device enumeration or GPU operation
occurred.

The actual compiler C ABI translates both entries from each container format.
Real SPIRV-Tools validates the results, then the separately frozen PS and NGG
adapters compile those exact SPIR-V bytes through Mesa NIR/ACO. The pipeline
uses explicit validated tool paths, without environment overrides or any
source substitution.

`build/apple-graphics-integration-1/report.json` records 36 successful checks
and four actual products. Standalone AIR and owned metallib produce identical
SPIR-V and machine code for each stage:

| Actual entry | Code / executable bytes | Machine-code SHA-256 |
| --- | --- | --- |
| `hn_triangle_vertex` | 144 / 124 | `ffa5d5142a6f8a5786296ba27f92e631c236bfd35161279b28919dd8997c9c61` |
| `hn_constant_fragment` | 56 / 36 | `15f6f976c7b77857915d8254f55d028c8413bcf74b96f8403767f1d90631f36c` |

The offline machine decoders check the actual NGG export guards and PS color
bit patterns. This is compiler evidence only: native graphics product
admission, complete owned draw state, draw submission, and pixel/guard
readback still need implementation and target validation. The earlier
compute frontend and fragment/NGG source freezes remain unchanged.

To reproduce in fresh evidence directories:

```powershell
C:/Python314/python.exe userspace/compiler/capture-apple-graphics.py --output build/apple-graphics-capture-new
C:/Python314/python.exe userspace/compiler/test-apple-graphics.py --capture build/apple-graphics-capture-new --ngg-compiler build/gfx11-ngg-local-3/hackynavi-aco-ngg.exe --fragment-compiler build/gfx11-fragment-local-6/hackynavi-aco-fragment.exe --output build/apple-graphics-integration-new
```

The capture command performs only offline compiler work on `victor@mini` and
must be coordinated with other Mac jobs to preserve its storage budget. Use
`--prepare-only` to write its exact source and capture script locally without
SSH. An unknown SSH outcome retains the pre-journaled remote stage; do not
delete it while a compiler could still be active.
