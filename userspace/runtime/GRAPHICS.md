# Reusable native graphics runtime

`Context::openGraphics` explicitly opens the real v1.5 IOKit profile and queries
its actual retained owner's color-layout observations. Existing `Context::open`
continues to request v1.4 compute. A graphics context retains the existing
serialized compute dispatch capability. There is no alternate transport,
software renderer, or automatic retry.

`GraphicsPipeline::capture` copies actual NGG vertex and fragment machine bytes
into its own zero-padded 4 KiB image and independently decodes that owned image
with `gfx11graphics::validate`. The supported program remains the narrow
resource-free Position/constant-RGBA profile. Caller JSON, compiler metadata,
and previous admission reports cannot substitute for instructions. The
immutable image survives input mutation/destruction; the kernel separately
copies and admits it on every real submission.

`graphicsInformation` returns numeric topology observed by the opening native
RPC. Applications may give it to the actual Mesa `hn_color_target_calculate`
adapter. It is not hardware ownership. `draw` independently checks every
canonical layout field against the retained observations, then the kernel
checks its own current same-owner topology and mappings again.

`draw` takes an existing Buffer from the same Context, a 4 KiB aligned offset,
and a linear RGBA8 layout. The logical target, including pitch padding, must fit
the Buffer's application length and remain at most 16 MiB. Its page-rounded
footprint must fit the real allocation. Exactly that footprint is mapped RW at
the Buffer's stable GPU address plus the offset; unrelated Buffer padding is
not mapped for the draw.

Before any upload or mapping, the context reserves four native BO slots and
8,667,136 bytes of resident budget: one 4 KiB unmapped upload, one private 4 KiB
code image, one private 8 KiB IB, and up to six 1,441,792-byte attribute-ring
segments. The native owner chooses real private control VAs and retains all
controls through completion or reset. The runtime never accepts caller PM4 or
maps its upload executable.

The ordered operation is actual allocate/upload, target mapping,
`graphicsProduct`, native fence wait, CPU history release, exact target unmap,
and upload release. A successful return publishes the real epoch/sequence only
after every step succeeds. The same target BO and its contents survive for
later draws, compute dispatches, or explicit download, with no implicit CPU
copy or reupload. Output parameters stay unchanged on failure. Any failed RPC
closes the actual owner through native retirement/recovery, even if hardware
may already have executed; it does not guess that DMA can be freed.

The host tests use the frozen actual Apple and authored ACO machine products.
They exercise immutable ownership and admission, invalid spans/stages, exact
image copies, target page rounding, every layout bit, offsets, logical versus
allocated lengths, and the 16 MiB boundary. They do not construct Contexts or
run a GPU. Local Darwin object compilation is source validation; native
linking, target draw execution and scanout are separate milestones.

`buildGraphicsPipeline` now uses the real vertex/fragment C compiler APIs in a
private temporary directory, reads bounded regular machine-code files and
independently admits their bytes. It publishes a pipeline only after removing
its exact owned files. The native ASan/UBSan integration in
`build/graphics-runtime-native-1` passed 24 checks over real Apple AIR/metallib,
including two complete compiler-backed pipelines and mutation/destruction
ownership checks. The linked compiler libraries are genuine native artifacts;
the sanitizer scope is the authored runtime/admission code and harness.

The complete Metal texture/pipeline/encoder frontend compiles and links in
`build/metal-graphics-native-2`. Its target render test has compiled, but actual
Metal discovery, drawing and readback on the Navi31 remain unverified.
