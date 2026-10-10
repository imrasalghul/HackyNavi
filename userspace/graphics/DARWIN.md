# Native graphics compiler and color-layout dependencies

`userspace/compiler/build-graphics-darwin.py` builds actual x86_64 macOS15
artifacts locally using the existing pinned Mesa archives. It does not contact
the Mac, rebuild Mesa, open a device, or run a Metal object.

```powershell
C:/Python314/python.exe userspace/compiler/build-graphics-darwin.py --output build/graphics-darwin-local-new
C:/Python314/python.exe userspace/graphics/prepare-native.py --native-relink --build-report build/graphics-darwin-local-new/report.json --compiler-report build/metal-explicit-tools-native-final-2/report.json --reader-report build/metal-system-reader-native-final/report.json --output build/graphics-darwin-native-prepared-new
```

Every output directory must be new. The original source stage, twelve archives,
compute compiler, and previous evidence are preserved. The script inspects every
archive member as a real x86_64 Mach-O object targeting macOS15.0; it rejects
Windows objects or an iOS platform. It hashes the actual archive bytes and all
included source, generated, and toolchain headers. A preflight compiler `-M`
scan must match the actual `-MD` dependency set and pre/post hashes. Reusing
these archives proves their identity and target; it does not reconstruct their
original source build.

Two limited source-derived adjustments make the layout dependency usable:

- The actual pinned `ac_surface.c` layout code is copied unchanged, retaining
  its license. Only its final unused `ac_surface_print_info` diagnostic function
  and `xf86drm.h` include are omitted. The genuine DRM UAPI header remains.
  The report records the original/generated hashes and removed function span.
- A fresh copy of addrlib replaces only `src_core_addrswizzler.cpp.o`, compiled
  from the same actual source with its supported `ADDR_ALLOW_SIMD=0` selection.
  This uses Mesa's real scalar implementation. It avoids the unavailable
  `__cpu_model` runtime dependency; no replacement CPU feature function exists.
  All fifteen other archive members must remain byte-identical.

The initial failed builds are retained at `build/graphics-darwin-local-1` and
`-2`; local cross-linking succeeded in `-3` and `-4`. Native validation of `-3`
then rejected extra exports before executing any fixture. The actual dyld
export trie, as well as `nm`, exposes twelve weak C++ allocation operators and
two linker symbols despite Zig's export-list option. Those cross-linked dylibs
are not approved for the frontend. The corrected `--native-relink` stage uses
Apple clang to relink the real objects/archives with exact export lists and
`-undefined,error`; the strict export check is retained. No Mach-O tables are
patched and no symbol check is weakened.

The second native link exposed a real libc++ ABI mismatch: the existing Mesa
objects use `std::__1::__hash_memory` from Zig's matching libc++ headers, which
the Mac SDK runtime does not supply. The successor captures the four real
Darwin runtime archives named by the actual verbose Zig link and verifies
every member target. Apple clang links these privately with `-nostdlib++` and
the same exact export lists. Their original archive bytes and LLVM/Zig licenses
are retained; no runtime function is invented or substituted.
The third native attempt caught a duplicate `__umoddi3`: Mesa addrlib already
provides its real Apple implementation. The color link therefore omits the
conflicting whole compiler-runtime object; the graphics link retains it. There
is no duplicate-symbol suppression or replacement implementation.

## Output contract

| Artifact | Interface |
| --- | --- |
| `hackynavi-aco-ngg` | `validated.spv entry private-empty-output-directory` |
| `hackynavi-aco-fragment` | Same arguments for the supported fragment profile |
| `hackynavi-aco-fragment-buffer` | Same arguments for the readonly fragment buffer profile; `--descriptor address` queries actual Mesa raw16 encoding |
| `hackynavi-aco-vertex-buffer` | Same arguments for actual VertexIndex-addressed positions; `--descriptor address` queries actual Mesa raw48 encoding |
| `libHackyNaviGraphicsCompiler.dylib` | Exactly six functions: the four `hn_gfx11_{ngg,fragment,fragment_buffer,vertex_buffer}_compile_validated_file` entries and two `hn_gfx11_{fragment_buffer,vertex_buffer}_descriptor` entries |
| `libHackyNaviColorLayout.dylib` | Exactly `hn_color_target_calculate`, `hn_color_target_validate`, `hn_color_target_validate_inputs` |
| `color-layout-tests` | Actual Mesa/addrlib calculations plus portable admission checks; no hardware access |

The stage compilers retain their existing headers under `userspace/compiler`.
The color ABI remains `include/hackynavi/gfx11_color_target.h`. Successful stage
compilation writes `shader.bin`, `shader.json`, and `aco.txt`. A caller must use
the real SPIRV-Tools validator first. AIR translation uses the existing explicit
`HNShaderTools` interface with absolute owned `apple-air-dis` and `spirv-val`
paths. No process-global environment configuration or PATH fallback is needed.

Both dylibs have `@rpath/<own-filename>` install names and only system runtime
dependencies. The graphics dylib hides Mesa internals and exposes only its six
C functions. The runtime `buildGraphicsPipeline` wrapper serializes those calls
under one mutex; callers must not bypass that serialization with concurrent
direct C ABI invocations. Neither compiler metadata nor numeric color topology
grants native buffer/submission authority. The kernel independently validates
private code, observed topology, layout, ownership, and the actual draw stream.

## Bounded native validation

The earlier two-profile `build/graphics-darwin-native-prepared-4` contains 188,118,200
input bytes and a 48,608,693-byte archive. It adds the 105.9MB verified Mesa
archive set, matching real Darwin runtime archives, and the exact real objects
needed for the two native dylib links. The bad
cross-linked dylibs are excluded. It uses the existing verified 32-export
shader compiler and fixed system AIR reader without rebuilding them. The
failed native runs remain at `build/graphics-darwin-native-1` through `-3`.
The successful actual Mac run is `build/graphics-darwin-native-4/report.json`:
52 compiler checks/eight real products and 249,778 layout checks/493 products
passed. The 251,414,616-byte stage was removed after verified export.
`product-wire-export-verification.json` independently checks the snapshot,
all artifact hashes, exact actual dyld export tries, executable export addresses
and byte-identical layout output.

The parent owns remote transfer/execution. Create a fresh private stage matching
`/Users/<owner>/Library/Caches/HackyNavi-GraphicsNative.<alphanumeric-suffix>`;
verify the archive hash before extraction and invoke:

```text
python3 <stage>/scripts/run_native.py <stage>
```

The runner verifies every input, performs the two actual Apple-clang links,
checks native install names/dependencies and the six/three exported symbol sets,
then translates the actual captured Apple AIR and metallib through explicit
tools and compiles each of the four profiles using both CLI and C ABI. All sixteen
real products must match the independently generated Windows code/metadata.
Three earlier unsupported fixtures and all 22 actual SPIRV-Tools-valid vertex
buffer negative programs must fail without successful products; each vertex
negative is checked through both CLI and C ABI. Six actual native Mesa raw48
descriptors must match the independent Windows descriptor results, including
addresses spanning 32-bit boundaries and the last valid 48-byte range. Misaligned,
null and overflowing descriptor requests must preserve output on failure.
The genuine vertex reflection has declared size 16 and unbounded array extent;
its four component accesses carry VertexIndex stride 16. The later trusted
three-vertex draw establishes the 48-byte bound, never reflection alone.
The real layout executable must
produce the same 493 Mesa/addrlib layouts as the existing Windows baseline.
Environment tool overrides are deliberately invalid during these tests.

Each native child runs in a process group with a 120-second deadline and bounded
termination/reaping. The runner never removes its own stage. Export
`native-report.json`, `results/compiler/report.json`,
`results/actual-layouts.bin`, and all seven artifacts; independently verify their
hashes. Remove only the exact verified stage after terminal completion and
`child_processes_reaped=true`. Unknown SSH status is not permission to delete
a potentially active stage. Input capacity is 208MiB and the remote stage budget
remains 256MiB. Preparation and transfer check compressed-plus-extracted storage
with 256KiB reserved for the bootstrap; the remote bootstrap checks the actual
combined size again. It removes only the redundant input tar after verified
extraction and before native linking, keeping outputs within the same cap.

The final outer native report should preserve the prepared `source_files` and
archive identity and attach the runner report as `native`, plus confirmed
`native_tests_passed`, `artifacts_exported`, `remote_job_finished`, and
`remote_stage_removed` fields. Frontend builders load each dylib from the final
report's directory and verify `native.artifacts[filename].sha256` and `bytes`.
A prepared or locally cross-linked report alone is not native execution proof.

`graphics_build_native.cpp` is the separate real C++ integration harness for the
frozen `GraphicsShader`/`buildGraphicsPipeline` slice. The actual successor
`build/graphics-runtime-native-1/report.json` passed 24 checks/two real pipelines.
Five authored C++ translation units were compiled with ASan/UBSan; the reused
compiler libraries were not sanitizer-instrumented. It checks actual AIR and
metallib translation, reversed-stage rejection, source/shader lifetime, exact
independent machine-code bytes and unchanged failure outputs. The production
read path includes `O_NONBLOCK` before regular-file validation. All six outputs
were exported and the 62,764,151-byte stage was removed.

Prepare a successor with `prepare-runtime-native.py`, then the local operator
can invoke `validate-runtime-native.py --prepared <prepared-directory> --output
<fresh-output> --no-agent`. Both require actual completed native dependency
reports; the latter transfers and runs only the immutable snapshot with a
96MiB remote stage cap. Neither constructs Metal objects or calls `Context::open`.
Compiler and layout success do not prove GPU rendering, scanout or Metal device
discovery.

The snapshot retains the existing compiler/Mesa license notices. Binary
redistribution must also retain the corresponding pinned source and notices;
the adapter's MIT license does not replace dependency licenses.
