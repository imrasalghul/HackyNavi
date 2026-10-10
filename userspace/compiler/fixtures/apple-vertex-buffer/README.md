# Captured Apple vertex-buffer shader

`vertex-buffer.metal` is the authored input. Apple Metal compiler 32023.864
compiled it with `-std=macos-metal2.4 -mmacosx-version-min=15.0`; Apple's
`metallib` tool produced the accompanying library. `provenance.json` records
the actual commands, host version, artifact lengths and SHA-256 digests.
Neither command executes the shader.

The shader returns `positions[vertexID]` from readonly `buffer(0)`. It has no
constant positions, colors, attributes, textures or instance input. Translation
reports a 16-byte element and an **Unbounded** array extent, with four scalar
accesses at `16 * VertexIndex + {0,4,8,12}`. The later narrow draw profile needs
48 accessible bytes because its trusted draw owner must enforce vertex IDs
0 through 2 and base vertex zero. That bound is not an Apple reflection claim.

The capture artifacts are compiler output from this repository's own small
shader. No Apple framework code, private shader cache or pretranslated
WindowServer shaders are included.

For compiler, machine-format and C ABI validation, see
[`docs/gfx11-vertex-buffer-compiler.md`](../../../../docs/gfx11-vertex-buffer-compiler.md).
