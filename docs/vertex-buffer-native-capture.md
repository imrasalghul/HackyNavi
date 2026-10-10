# Native vertex-buffer graphics capture

The native driver and userspace runtime have additive vertex-buffer draw paths.
One path combines the actual buffer vertex shader with a constant-color
fragment shader; the other also reads the actual fragment buffer. Both still
draw exactly one triangle, vertices 0 through 2, base vertex zero and one
instance. This connects the bounded shader/draw validators to real allocation,
capture, submission and retirement code. It does not demonstrate a successful
target draw or Metal acceleration.

## Wire contract

The explicitly negotiated profile is v1.7:
`kVertexBufferedGraphicsMinor` and `kVertexBufferedGraphicsCapabilities`.
It includes the earlier reusable compute/graphics capabilities and adds
`PositionVertexBufferGraphics` and `PositionVertexFragmentBufferGraphics`.
Earlier v1.5/v1.6 selectors and wire sizes remain unchanged. New requests cannot
be accepted on an older negotiated connection.

| Selector / record | Size | Resources supplied by the caller |
| --- | --- | --- |
| `ProductVertexBufferedGraphics` | 160 bytes | upload code, target, readonly vertex buffer |
| `ProductVertexFragmentBufferedGraphics` | 184 bytes | upload code, target, readonly vertex and fragment buffers |

`ProductVertexBufferedGraphicsRequest` embeds the existing draw record and adds
`vertex`, `vertexOffset`, and `vertexBufferBytes`. The last field must be 48;
`draw.vertexBytes` separately describes the shader's machine-code length.
The combined record embeds this as `vertexDraw` and adds `fragment`,
`fragmentOffset`, and `fragmentBufferBytes`, which must be 16. Resource offsets
are 16-byte-aligned backing-BO offsets. Overflow, stale connection epochs,
missing capabilities, invalid handles, reserved fields and identities shared
by code/target/vertex/fragment are rejected before capture.

The wire carries no descriptor, physical address, kernel pointer, topology
authority or caller-created completion proof. The 48-byte vertex requirement
comes from the narrow draw, not the shader's Unbounded Apple reflection.

## Actual ownership and retirement

`HackyNaviClientSession` captures the already copied wire record under its
session lock. It validates the negotiated profile and logical BO ranges,
requires upload code to be unmapped, obtains the existing real FunctionGroup
Operation and GART access, and copies the uploaded shader image into private
CPU storage. Submission uses the same existing `completeCaptured` path and
owner-failure containment as earlier graphics products.

`HackyNaviVMContext` resolves the vertex and optional fragment handle against
its actual current BO/mapping records. The required span must lie inside the
one full readonly mapping. The retained binding keeps that mapping's complete
VA extent, backing-BO offset, allocation extent, ID, generation, owner and
epoch. The draw-relative resource offset is derived by subtracting the actual
mapping offset from the requested BO offset. It is not assumed to be zero or
page aligned. The independent draw builder derives descriptors exposing
exactly 48 vertex bytes and 16 fragment bytes.

The code image, attribute ring and IB are newly allocated private controls.
The complete capture has four, five or six bindings according to its explicit
shader class. Every retained binding has one exact mapping with the expected
permissions; the independent draw validation rejects duplicate storage IDs and
overlapping full VA extents. Native storage checks additionally verify distinct
CPU and DMA intervals across all resident BOs and page-table storage.

Sealing prevents CPU writes and mapping changes while work may consume these
buffers. Capture validation rechecks the same owner, generation, topology,
bindings, actual shader bytes and canonical command stream before use. The
same real queue completion/readback/consumption route releases private controls
while retaining user data BOs. Ambiguous or exposed failures keep the existing
retirement ownership; no new cleanup path claims completion or retries a draw.

## Runtime API

`VertexBufferGraphicsPipeline` and `VertexFragmentBufferGraphicsPipeline`, in
`graphics_buffer_snapshot.hpp`, own immutable copies of actual machine code.
Their capture routines independently decode the copied bytes with the matching
shader validator. The two types cannot silently exchange fragment resource
contracts. A compiler metadata report or shader hash does not grant admission.

`Context::draw` overloads accept the matching immutable pipeline, target and
layout, vertex buffer/offset, and optionally fragment buffer/offset. Before
allocating or mapping anything, they check the same actual Context owner and
epoch, live reservation slots, distinct resource identities, logical bounds,
VA extents and resident/control budget. They retain the buffers, map the full
resource allocations read-only, submit the matching new selector, wait for
the actual native fence, release historical output, unmap resources and release
the staging upload. Failure closes the context without retry; completion output
is assigned only after successful cleanup.

`openVertexBufferedGraphics` and `openVertexBufferedGraphicsAccelerator` select
v1.7 explicitly. The accelerator route retains the supplied actual accelerator
port, verifies its immediate native owner, opens that same port, authenticates
Hello against the owner ID and checks the relationship again. It has no direct
owner fallback. The previously reviewed v1.6 accelerator open/start paths are
preserved; the new entry points do not change their identity or lifetime rules.

## Evidence and limits

`build/vertex-native-compiled-2/report.json` records six actual kernel-header
compilations with `-Werror` and a 2 KiB frame limit, plus x86_64 Darwin Context
and snapshot objects. It binds all current source/dependency hashes. The IOKit
transport requires a separate native macOS SDK compilation.

`build/vertex-runtime-tests-2/report.json` records 8,522 focused checks in both
strict and UBSan builds, using genuine recorded Apple/ACO vertex and fragment
products. Tests cover immutable ownership after inputs are destroyed, actual
machine mutations, exact class separation, wire truncation/alignment, resource
extent and identity rejection, negotiation and older selector compatibility.
The existing native ABI suite also passes all 160,058 checks in both builds.
There is no fake IOKit transport or GPU execution model in these tests.

Target engine bring-up, actual buffer draws and pixel readback remain required.
Passing compilation, CPU shader admission, wire validation or the earlier
read-only target capture does not establish those outcomes.
