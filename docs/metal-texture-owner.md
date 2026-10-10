# Native RGBA8 texture owner

`userspace/metal/native/HNMetalTexture.hpp/.mm` implements a standalone Metal
texture over the real reusable `compute::Buffer`. It does not open another
device, expose a CPU heap as GPU memory, or claim a rendering result.

The constructor accepts the device's exact retained `compute::Context`, checks
the actual v1.5 `graphicsInformation` result, and invokes
`hn_color_target_calculate` from the pinned Mesa/addrlib dependency. It then
validates that output and allocates the real native BO. The returned logical
and page-rounded allocation sizes must match exactly. Native DMA allocation
clears the entire allocation before publication; the optional Managed CPU copy
is separately zero-initialized.

The current profile is `MTLTextureType2D`, `MTLPixelFormatRGBA8Unorm`, one layer,
mip, sample and depth slice, identity channel swizzle and `RenderTarget` usage.
Width/height are bounded by 16384, but the native 16 MiB BO limit also applies
to the actual padded footprint. Storage is Private or Managed, CPU caching is
Default, and hazards are Default/Tracked. Shared, memoryless, lossy, placement
sparse, shader sampling/writes, views, IOSurface, heaps, peer sharing and
argument-buffer resource IDs are not implemented and are explicitly rejected.
The Lossless enum expresses preservation of pixel values; no compression or
DCC capability is claimed. The requested GPU-optimization permission is
reported, while this implementation always retains its validated linear layout.

## CPU transfers and ordering

Managed `replaceRegion` snapshots requested pixels into packed owned rows,
uploads each exact row through the actual native buffer and commits the CPU
cache only after every upload succeeds. Intervening padding and neighboring
pixels are never uploaded from a potentially stale CPU copy. A failed upload
closes the actual context; GPU rows may have changed, but the CPU cache stays
unchanged. There is no rollback or success claim after partial transfer.

Managed `getBytes` copies the CPU cache. A command encoder must explicitly
record `synchronizeManagedContents` after earlier real work to update that
cache. This uses `Buffer::download`, which commits only after all actual native
reads succeed. Private CPU transfer methods reject the request. Nothing
implicitly substitutes CPU-generated pixels for a render result.

The real SDK declarations are captured in
`build/metal-texture-sdk-capture/MTLTexture.h` and related headers. Apple's
[replaceRegion documentation](https://developer.apple.com/documentation/metal/mtltexture/replace(region:mipmaplevel:slice:withbytes:bytesperrow:bytesperimage:))
requires zero image stride for this non-3D profile and permits zero row stride
for a single source row. The implementation checks the documented read/write
row-stride limits and pixel-size multiples. The
[getBytes documentation](https://developer.apple.com/documentation/metal/mtltexture/getbytes(_:bytesperrow:bytesperimage:from:mipmaplevel:slice:))
describes an immediate CPU copy, separate from GPU synchronization. Retrieved
source documents and hashes are preserved in `build/metal-texture-local-1`.

The production `texture_region` helper checks region bounds, integer and
pointer-span overflow, host stride, texture extent, and all copy aliases before
the first write. Copies touch only pixel rows. No partial output is published
on helper validation failure. Tests execute these actual copy routines over
real CPU byte arrays, checking outside pixels, both kinds of row padding,
guards, malformed spans and maximum-dimension arithmetic. Their numeric
layouts are geometry test inputs, not claimed Mesa or hardware observations.

## Integration seam

The device factory creates `HNMetalTexture` with its existing actual graphics
context and the application descriptor. The class retains that device,
context, native buffer and immutable calculated layout. Plain `init` fails,
and no borrowed descriptor or input pointer survives construction/transfer.

The render command owner must retain the exact texture object, verify its
device/context identity, and call `drawWithPipeline:completion:` in queue order.
This method holds the texture mutex across actual `Context::draw`. It returns
only that backend's result and completion; it does not create sequence numbers.
The same mutex serializes Managed uploads and downloads. Since there are no
views and no native-buffer accessor, another texture cannot bypass this
ownership through shared storage. Other textures can share the Context's
already serialized queue without acquiring this texture's lock.

The blit command owner records `synchronizeManagedContents` for the retained
Managed texture and executes it in order. The two integration files
`HNMetalDevice` and `HNMetalCommand`, and shared build lists, remain owned by
the parent workstream. New native link inputs are `HNMetalTexture.mm`,
`texture_region.cpp`, and the real color-calculator library; there is no
replacement color calculator or fallback implementation.

The focused local tests establish CPU transfer properties. Actual macOS SDK
compilation is a separate validation step. Neither compilation nor these tests
instantiate a Metal object, execute a draw, prove target boot readiness, or
advertise general Metal rendering support.
