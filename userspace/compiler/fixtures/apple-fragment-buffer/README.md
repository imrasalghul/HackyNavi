# Actual Apple fragment buffer fixture

The authored MIT `fragment-buffer.metal` reads `constant float4& color
[[buffer(0)]]` and returns MRT0. Its color is supplied by a buffer; the source
contains no replacement output constant.

The retained AIR and metallib were produced by installed Apple Metal
**32023.864** on macOS **15.8 / 24H22**, with `-std=macos-metal2.4
-mmacosx-version-min=15.0`. `provenance.json` is the exact successful
compiler-only capture report, including actual commands, output hashes,
export verification and removal of the 7,123-byte temporary stage.

Both containers translate identically through the actual explicit-tools shader
API: one read-only buffer0/set0/binding0, declared 16 bytes, static range `[0,16)`.
Actual SPIR-V loads four u32 components at offsets0/4/8/12, bitcasts to float and
stores RGBA. The separate `fragment-buffer.c` adapter preserves these reads.

Evidence is in `build/apple-fragment-buffer-capture-1`,
`build/apple-fragment-buffer-translation-1`,
`build/gfx11-fragment-buffer-compiler-4` and
`build/gfx11-fragment-buffer-tests-4` (873 checks).

No Metal objects or GPU commands were executed. Existing native graphics
admission remains resource-free; descriptor, BO lifetime, submission and Metal
binding support are separate work.
