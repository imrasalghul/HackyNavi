# Public Metal vertex-buffer rendering

The frontend now selects native profile v1.7 through the actual accelerator
port. It supports a single readonly vertex `buffer(0)` containing three
`float4` positions, with either a constant fragment color or the existing
readonly fragment `buffer(0)` color. This is a restricted offscreen triangle
path. Target GPU execution and general Metal compatibility remain unverified.

The actual Apple vertex AIR/metallib translates through the real AIR reader,
SPIR-V validator and pinned Mesa/ACO adapter. Reflection describes a 16-byte
element with unbounded VertexIndex-strided accesses. The 48-byte accessible
range comes from the supported draw (three vertices, start/base zero, one
instance), not a fabricated finite reflection extent. Both machine-code
profiles pass their own independent kernel instruction admission.

`setVertexBuffer:offset:atIndex:` and `setVertexBufferOffset:atIndex:` require
index zero, a 16-byte-aligned offset and 48 bytes within the actual logical
buffer length. A nil binding requires offset zero. The combined profile also
requires a distinct fragment BO with 16 accessible bytes. The encoder records
strong references and offsets per draw; clearing/rebinding afterward cannot
change that draw. Full command preflight, Context ownership, private native
capture and exact readonly mappings apply before submission. Capabilities add
one vertex buffer only for the exact v1.7 contract; vertex attributes, indexed
drawing, textures and general feature sets are not advertised by this change.

The four graphics pipelines remain distinct immutable types: constant,
fragment-buffer, vertex-buffer and combined vertex/fragment buffers. The real
compiler calls share the existing process-wide compiler mutex. Actual machine
bytes are copied into owned images and checked independently before use.

## Target-only smoke tool

`userspace/metal/native/render-vertex-buffer-smoke.mm` is compiled on the build
Mac, never run there. On a future engine-ready target with the matching
accelerator/frontend installed, it requires explicit physical identity and
exact genuine Apple fixture hashes:

```sh
./hackynavi-metal-render-vertex-buffer-smoke --run --registry ACCELERATOR_ID \
  --vertex-metallib "$PWD/vertex-buffer.metallib" \
  --constant-metallib "$PWD/graphics.metallib" \
  --fragment-metallib "$PWD/fragment-buffer.metallib"
```

This command is not the next boot instruction: the inherited-display
reservation gate still needs resolving first. The test discovers the public
Metal device and checks the exact accelerator/owner/PCI chain; it has no
fallback device or direct Context construction.

For each of the vertex-only and combined pipelines it uploads two shapes to
the same real Managed position buffer. Offset16 contains a left-half triangle;
offset80 changes from a degenerate triangle to a right-half triangle on the
second upload. The draws capture offsets16/80 respectively, then the encoder
is changed or cleared. The last draw releases the application's source
references before committing. Combined draws also change the uploaded color.
Missing, misaligned, short, wrong-index and shared-BO bindings must reject.

Each actual draw is followed by ordered target synchronization and completion.
The tool checks real 64x64 RGBA8 readback against the independently computed
triangle interior/exterior, a narrow edge exclusion band, orientation, padding
guards, callback counts and different output hashes. CPU calculations only
check the returned pixels; they never populate the target with a rendered
triangle. An exit-zero target report would establish these particular cases,
not scanout, WindowServer or full Metal application compatibility.

## Evidence

- `build/graphics-vertex-shader-local-1/report.json`: actual AIR/metallib
  reflection/ownership tests, 4,027 checks in each strict and UBSan build.
- `build/vertex-runtime-tests-2/report.json`: new immutable images and wire
  validation, 8,522 focused and 160,058 existing checks in each build.
- `build/vertex-native-freeze-1/report.json`: six real kernel-header units and
  two Darwin runtime objects with source/dependency hashes.
- `build/vertex-metal-review-1/report.json`: independent frontend ownership,
  binding and compiler route source review, no actionable finding.
- `build/vertex-buffer-native-transport-1/report.json`: actual macOS SDK
  transport compilation and sanitizer protocol run; remote stage removed.
- `build/vertex-psp-integration-1/report.json`: 1,119 capability checks under
  strict and UBSan builds, plus the new CMake targets.

- `build/graphics-vertex-buffer-darwin-native-1/report.json`: actual Apple
  native relink, exact six graphics exports, 270 compiler checks across 16
  genuine products, 22 unsupported VBO inputs and six raw48 descriptors. The
  existing color oracle passes 249,778 checks over 493 real Mesa products.
- `build/graphics-vertex-runtime-native-1/report.json`: 96 ASan/UBSan checks
  across eight actual AIR/metallib-to-machine-code pipeline constructions.
- `build/vertex-frontend-native-1/report.json`: full 22-unit frontend and four
  public target tools compile/link on the real SDK. The vertex tool is included
  and was not run. Bundle SHA256
  `ff472f87886e463f9ac1eef57cac83a91a651913c594c5f8bfbc2520ed79bcab`.

All three native stages completed and were removed. None of these checks is a
target GPU result.
