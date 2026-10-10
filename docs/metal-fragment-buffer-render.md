# Public Metal fragment-buffer rendering

The Metal frontend opens the exact native v1.6 buffered-graphics profile. Its
existing constant-color and compute paths remain available. A fragment function
is admitted through the new compiler route only when the real owned reflection
describes the supported readonly `buffer(0)` float4 at byte offsets 0–15. The
immutable `FragmentBufferGraphicsPipeline` contains actual compiled GPU code;
the earlier resource-free pipeline remains a distinct type and admission path.

`setFragmentBuffer:offset:atIndex:` and `setFragmentBufferOffset:atIndex:` accept
only index zero, a 16-byte-aligned offset, and a complete 16-byte logical range
inside an actual `HNMetalBuffer` owned by the same Context and Device as the
render target. Nil with index/offset zero clears the binding. Invalid setters
leave the previous binding unchanged. Each draw records strong references to
the Metal buffer and its native BO plus that draw's offset; subsequent rebinding
does not alter the recorded command. The complete command buffer is preflighted
before submission, including the exact native identity and range checks.

Managed `didModifyRange:` performs the existing real upload before returning.
The command FIFO invokes the actual native buffered draw with the retained BO;
it does not inspect the CPU color or render into a CPU image. Texture drawing
and region I/O share the texture mutex. Ordered `synchronizeResource:` downloads
the actual target before `getBytes:` exposes its Managed CPU copy. Native
Context serialization, mapping permissions, fence/drain/detach and job-resource
retirement remain enforced below this frontend. Applications must not race CPU
buffer writes against GPU use.

This is the existing one-triangle, RGBA8Unorm offscreen profile extended by one
readonly fragment float4. It does not add vertex buffers, sampled textures,
argument buffers, buffer arrays, indexed drawing, or general Metal feature-set
support. Capability metadata advertises exactly one fragment buffer only for
the exact v1.6 capability set.

## Validation and target test

`build/metal-render-buffer-local-1/capabilities-report.json` records 901 strict
checks and 901 UBSan checks of actual portable capability admission, with zero
failures. These are offline metadata tests. Objective-C compilation and actual
target rendering are separate requirements; neither is established by those
test counts.

`userspace/metal/native/render-buffer-smoke.mm` uses only public Metal calls and
read-only registry identity checks. It requires an explicit `--run` and the real
HackyNavi accelerator ID. Build hosts must compile this executable only.

```text
render-buffer-smoke --run --registry ACCELERATOR_ID \
  --vertex-metallib /absolute/graphics.metallib \
  --fragment-metallib /absolute/fragment-buffer.metallib
```

The exact original Apple compiler fixtures are:

| Fixture | Bytes | SHA-256 |
| --- | ---: | --- |
| `userspace/compiler/fixtures/apple-graphics/graphics.metallib` | 6354 | `b99ae2fa65e9cfda2e13c146434fee1f0c0ef0cd2956beeba1cec01750641197` |
| `userspace/compiler/fixtures/apple-fragment-buffer/fragment-buffer.metallib` | 3496 | `d5feaa1df63810fbb0ee3013497d01cb814f7cbb02a99c32ea25867859acdefd` |

The test uses one pipeline and one Managed source BO for two different real
uploads. It tests missing, unaligned, out-of-range and wrong-index bindings,
records offsets 16 and 32, changes/clears encoder bindings after recording, and
releases its source reference before the second commit. Both passes queue real
draws and target synchronization, wait for actual completion, and check uploaded
colors, untouched exterior pixels, Metal Y orientation, host row guards and
callback counts. A poison value in the first upload prevents the second pass
from passing solely by selecting a different offset in the old upload. The
actual pixel hashes must differ. A compiled executable is not a passed GPU
test; only a successful target run can supply that evidence.
