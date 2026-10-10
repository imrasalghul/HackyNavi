# Native Metal integration: measured boundary and next implementation

Current graphics integration (2026-10-11): `build/metal-graphics-native-2`
compiles all 21 frontend units and links the real bundle and dylib against the
native SDK. All 41 commands completed without diagnostics. This includes
stage-aware libraries/functions, actual Mesa/ACO graphics compilation,
`HNMetalRenderPipeline`, real Private/Managed RGBA8 textures, a serial render
encoder and ordered texture readback. The supported draw is one resource-free
triangle; general resources, presentation and desktop acceleration remain work.
Both public-Metal target test programs compiled but have not run. No Metal
object was instantiated on the build mini.

`build/graphics-darwin-native-4` validates the real graphics compiler and color
layout libraries: 52 actual AIR/metallib checks and 493 layout checks. The
authored C++ shader-to-pipeline path passed 24 actual compiler-backed ASan/UBSan
checks in `build/graphics-runtime-native-1`, including lifetime ownership after
input and shader destruction. The reused compiler libraries themselves were
not instrumented. Each native stage was exported and removed. This is offline
execution evidence, not GPU output or Metal device discovery.

The earlier complete 14-unit frontend compiles and links against the actual SDK in
`build/metal-device-native-2`, including the genuine `_MTLDevice` subclass,
owned capabilities, architecture metadata and a real MH_BUNDLE export.
All 31 commands pass without diagnostics; root independently verified 40
input members, all 14 archive objects and four artifacts. Stage removed.
This combines the Managed and explicit-tool paths described below. It does
not establish NSBundle loading, Metal discovery, GPU execution or rendering.
The separate fixed-system AIR helper in `build/metal-system-reader-native-final`
passed real AIR/invalid input/environment-redirection checks and 94 actual
compiler ABI checks with the unchanged verified compiler.

Static compiled Objective-C inspection and packaging are now complete in
`build/metal-bundle-package-final-3`. All 391 implemented required method
encodings match the measured contract; one required notification hook is
inherited from the genuinely bound framework superclass. Root verified 88
package files, 4,198 source members and all five unchanged native binaries.
Archive SHA-256:
`c78cceec88dfb01d1337a8576fab3d573fbae036482cb6af39e3704599aecd43`.
This package is a development artifact; it has not been installed or loaded.

Explicit tool context now passed the actual native compiler suites:
`build/metal-explicit-tools-native-final-2` has 32 exports, 94 new C ABI checks
and 27 C++ ownership checks under ASan/UBSan. Device and Library copy the
bundle's absolute AIR disassembler/SPIR-V validator paths into each shader;
later pipeline validation retains those same paths. Root verified all 44 input
members and exported artifacts; the temporary stage was removed. This validates
the compiler/runtime path, not Metal discovery or shader execution on the GPU.

Latest Managed integration: `HNMetalBuffer` owns a page-aligned CPU copy and
actual native GPU allocation. Initial bytes and `didModifyRange` use real
uploads; the command stream now retains and executes blit `synchronizeResource`
readbacks at their exact position after earlier compute returns. Failed reads
leave the CPU copy unchanged. This follows the explicit-copy
[Managed resource contract](https://developer.apple.com/documentation/metal/synchronizing-a-managed-resource-in-macos).
Private and Managed buffers accept default caching and tracked/default hazards;
Shared mappings and arbitrary blits are not implemented. The subsequent
graphics integration adds the texture storage described above.

Independent reviews bind the Buffer/runtime and mixed command freezes in
`build/metal-managed-buffer-local` and `build/metal-managed-command-local`.
The native snapshot `build/metal-managed-native-1` compiled and linked eleven
actual objects and built `hackynavi-metal-compute-smoke`. Its 22 commands
passed without diagnostics. Parent validation checked 32 input members,
eleven static archive objects and all three exports; stage removed. The smoke
uses public Metal discovery and checks actual A-to-B-to-C output, retained B,
4099-element tail handling, 64 guard words, ranged upload, readback and callbacks.
It has not executed. This snapshot explicitly excludes the still-evolving
device/capability layer and predates explicit compiler-tool context integration.
Future build runners reject nonempty output directories and include the final
artifact archive in storage measurements; the successful original evidence
is preserved unchanged.

Earlier command integration: actual `HNMetalCommandQueue`, command buffer and
compute encoder compile under the real SDK's strict ARC checks and link with
the native runtime. `build/native-compute-command-final` passed 55,881 native
host lifecycle sanitizer checks. Parent verification checked all 37 inputs,
six exported artifacts and nine unchanged object byte sequences; the temporary
stage was removed. This code records and submits real compute work through
Context, but no instance has yet been constructed or run on the target.
The next frontend uses the measured `_MTLDevice` base and the public
IOAccelerator child; see [loader contract](metal-loader-contract.md).

Update 2026-10-11: native reusable compute v1.4 is built in
`build/macos-reusable-v14-final`: 92 translation units, 406 resolved imports,
234 verified source files, executable SHA-256
`67fc0db187d72a5fd459194924d0f17b91dd6f20c3a08c69eee6d19863ea8cb6`.
The real Session/IOUserClient path preserves data BOs after queue-proven job
consumption, reactivates the detached VM, and retains up to eight CPU results.
Native transport tests passed 158,417 checks under ASan/UBSan. The focused and
full build stages were removed. This has not run on the target GPU. See
[current integration evidence](validation-product-wire-power.md).

The subsequent `userspace/runtime` layer owns immutable compiler products and
persistent native buffers, with explicit copied I/O, stable VA reservations,
serialized dispatch and native cleanup. Real compiler-backed native tests in
`build/native-compute-runtime-final/report.json` passed 233 ASan/UBSan checks,
including code ownership after freeing original AIR/pipeline objects and all
eight exact-thread regions. The 60,083,757-byte build stage was removed. The
target-only context smoke is linked but has not run. The later buffer identity
correction passed its focused native relink in
`build/native-compute-runtime-identity-final/report.json`. The four unchanged
C++ objects were verified byte-for-byte, all new artifacts were exported and
the 40,960,697-byte stage was removed. The earlier 233 tests were not repeated.
That earlier `userspace/metal/native/HNMetalBuffer` milestone added private
storage requiring the driver's actual retained context; later milestones above
add Managed storage and the graphics objects. Shared storage remains absent. The library
parser/compiler is now native-validated for measured MTLB 2.5 graphics and
MTLB 2.8 macOS15 compute fixtures;
see [its supported format and evidence](../userspace/metal/METALLIB.md).

`HNMetalLibrary` and compute `HNMetalFunction` now compile and link natively
against the owned metallib ABI. Their retained `compute::Shader` builds actual
full/tail ACO variants and preserves compiler diagnostics. The focused run in
`build/native-compute-shader-library-final-2/report.json` passed 252 ASan/UBSan
checks across five cases and fourteen variants. All artifacts were verified
and the 60,328,604-byte stage was removed. Library parsing and offline shader
tests do not establish Metal object construction, device discovery or GPU
execution. The installed Apple Metal Toolchain works when explicitly selected:
a real macOS15/MSL2.4 compute AIR/metallib pair is now captured with provenance.
The MTLB2.8 parser refresh passed 460 existing and 48 new native ABI checks in
`build/metal-compute-metallib-native-final`. The command-line toolchain is not
yet a production implementation of `newLibraryWithSource:`.

`HNMetalComputePipeline` now compiles and links natively. It compiles an actual
prototype, checks native-profile reflection and occupancy, and compiles and
validates each dispatch shape through the shared kernel ISA validator.
`build/native-compute-pipeline-final` passed 265 ASan/UBSan checks across nine
real cases and twenty full/tail variants. Seven previous objects were verified
byte-for-byte; all six exported artifacts were checked and the 60,486,732-byte
stage was removed. This validates the compiler/admission boundary and object
compilation, not a constructed Metal pipeline or GPU execution.

The [discovery investigation](metal-discovery-prerequisites.md) now also uses
bounded actual macOS15 method disassembly and kernel-collection metadata.
Public Metal registry IDs name the accelerator service, while the current
native context registry ID names the separate driver owner. The private
context-owner interface preserves that distinction; the future device factory
must validate their service relationship instead of rewriting/comparing the
two IDs as though they were the same service.

This audit covers the current source and focused build evidence. HackyNavi has
an implemented native path for one compiler-produced compute class, but that
ACO product has not yet been executed on the macOS Navi31 target. There is no
HackyNavi `MTLDevice`, Metal driver bundle, Metal command encoder, or demonstrated
WindowServer acceleration. A successful compiler or transport build is not
Metal discovery or GPU execution evidence.

## Sources and actual OS evidence

The NVIDIA reference is pinned to `508f3d2496e78a9640d59128a56ddb5a53061606`;
Cobalt64 to `d6c9aedf8e6526a39e349b5886cbc8c7ddf77f65`. Neither project was built,
installed or executed for this audit. Their source demonstrates their own
integration choices, not an Apple-supported third-party driver API.

The development mini reports **macOS 15.8, build 24H22**, and its installed SDK
is **26.2**, not a macOS 15 SDK. The user target is reported as 15.8.1 and has not
been inspected by this audit. The bounded read-only capture at
`build/metal-native-audit/sdk-runtime.json` records exact header hashes, line
numbers, OS/SDK versions, and actual Objective-C runtime method encodings. It
created no remote files, enumerated no devices, instantiated no device or
queue, and submitted no GPU work. Exported classes/selectors are evidence of
their existence on the mini; they do not prove initialization preconditions,
private object layouts, or compatibility with the user's exact build.

Apple's public declarations in the captured `MTLDevice.h` expose
`MTLCreateSystemDefaultDevice` at line130, `MTLCopyAllDevices` at137, and
`registryID` at459. The captured `Metal.tbd` lists `MTLIOAccelDevice` at249.
On the actual 15.8 runtime, that class exists, has superclass `_MTLDevice`, and
its `initWithAcceleratorPort:` encoding is `@20@0:8I16`: object return and one
32-bit unsigned argument. `_MTLCommandQueue` and `_MTLCommandBuffer` also exist.
No public provider-registration API was identified in these declarations.

## What discovery actually needs

NVIDIA's [accelerator personality](https://github.com/nullmoth/nvidia-macos-driver/blob/508f3d2496e78a9640d59128a56ddb5a53061606/kexts/NVRM/accel/Info.plist#L35)
sets `IOMatchCategory=IOAccelerator`, `MetalPluginName`, and
`MetalPluginClassName`. Its `nvrm-accel.cpp:453-486` subclasses/starts the
accelerator family before registering the service. The bundle class in
`plugin/NVMTLDevice.m:133-137,344-358` subclasses `MTLIOAccelDevice`, calls its
real initializer, and rejects initialization when its actual NVK backend
cannot open. This is more than adding three registry strings to an IOService.

Cobalt's `tools/native/navi48metal/Navi48Device.m:73-104,6237-6250` uses the
same exported base class and initializer, with its own accelerator/client
integration. Its bundle `Info.plist` declares `NSPrincipalClass=Navi48Device`
and **minimum OS26.6**. Its source comments include observed private runtime
behavior from that system. These are useful leads for investigation, not
Sequoia vtable, object-size, or selector-behavior authority. In particular,
neither its private class offsets nor its Navi48 registers can be transplanted
into this Navi31/macOS15 implementation.

Our `kext/HackyNavi/Info.plist:25` publishes `HackyNaviDevice`, and
`HackyNaviDevice.cpp:784` registers that service. It is currently the endpoint
for our explicitly typed native user client. It does not declare a Metal
plugin, implement the accelerator-family object graph, or instantiate a Metal
device. Existing real DMA/VM ownership must remain in that owner even when an
accelerator-facing service is added later; another PCI owner cannot start a
second independent engine/VM lifetime.

The next discovery work is to establish the actual 15.x accelerator initializer
and user-client requirements from the target runtime, implement the required
family contracts, and bind a bundle to that exact live owner. An ordinary
`NSObject<MTLDevice>` in a test process, interposing `MTLCreateSystemDefaultDevice`,
or returning an object with fabricated feature support would not establish this
discovery path. The public `MTLDevice` protocol alone is not a provider loader.
Do not publish Metal plugin properties until the real bundle can initialize
its backend and supported object paths; initialization failures must return nil
with diagnostics instead of a successful but nonfunctional device.

## Executable compute: implemented versus missing

| Boundary | Implemented source/evidence | Remaining work |
|---|---|---|
| AIR to SPIR-V | Actual AIR/LLVM translation, SPIRV-Tools validation, exact function/stage/reflection and bounded measured MTLB2.5/2.8 extraction with an owned function C ABI; native compiler exports31 functions | Other container profiles, production source-to-AIR path, supplied function constants and wider shader semantics |
| SPIR-V to Navi31 ISA | `userspace/compiler/compiler.c:116,153,295-370`: real NIR/ACO compute lowering, explicit gfx1100/wave32 ABI and metadata | General resources and specialization; current output rejects scratch, LDS, constants/relocations and unsupported NIR operations |
| Fixed compiler product | `userspace/compiler/fixtures/apple-add-one/manifest.json`, verified104-byte original ACO output | This is not arbitrary dynamically compiled program admission |
| Native execute/readback/retire | Immutable controls, retained mappings, queue fence/RPTR, copied readback and VM retirement; v1.4 and `userspace/runtime` preserve resources across completed jobs | Target execution evidence, multiple dispatches/pipelines per Metal command buffer, asynchronous scheduling and broader recovery validation |
| Dispatch planning | Actual translator regions, real specialized ACO full/tail variants, immutable runtime products and native multi-region submission are connected in source | Target execution of full/tail regions and wiring to Metal encoders |
| Metal API objects | Native-compiled private `HNMetalBuffer`, `HNMetalLibrary`, compute `HNMetalFunction` and `HNMetalComputePipeline` use native storage, owned container AIR and validated actual compiler results | Target object construction, shared/managed storage, device/queue/encoder implementations and discovery integration |
| Presentation | Separate display/scanout work exists in the driver | Metal textures/render pipelines, IOSurface import/lifetime/coherence, drawable presentation, display synchronization and WindowServer integration |

The current native v1.2 class accepts two owned BO handles and offsets. It runs
4,096 uint additions with a64-thread workgroup and reads16,640 bytes, including
64 untouched guards. It accepts no caller code, PM4, descriptors, register
values, or GPU pointers. Source inspection and native compile evidence show a
real dispatch path; `hackynavi-compiler-smoke --run` still needs an authorized
run on the Navi31 target to establish its behavior.

The existing Apple AIR capture helper is not a driver-side MSL compiler:
`userspace/metal/tools/capture_apple_air.m:21-37` first obtains an existing
system Metal device and uses its compiler plus the private `bitcodeData`
selector. It cannot bootstrap Metal on a machine where no usable system device
exists, and an unavailable selector must remain an explicit failure. A general
`newLibraryWithSource:` implementation needs an actual source compiler service
or a validated independent compiler route; accepting only the saved add_one
source would not implement that API.

## Component implemented in this audit: planner C ABI

`hn_shader_plan_dispatch` consumes a live successful `HNShaderResult`, an
explicit dispatch kind, and three-dimensional grid/local dimensions. It reads
the actual retained reflection and delegates exact-thread splitting to
`vendor/metal2vulkan/src/reflect/mod.rs:92-180`. It produces at most eight owned
region records with group counts, local sizes, bases and the exact48-byte push
payload. Accessors copy a116-byte C record; there are no borrowed data pointers
inside it. The original compiler result can be freed after construction. Error
plans contain no regions, failed region reads leave output unchanged, and
successful zero-size grids produce no dispatch regions.

There is an important source-backed distinction:
`vendor/metal2vulkan/src/passes/mod.rs:1251-1286` emits **literal local-size
constants for Workgroups**, and specialization IDs0,1,2 for exact-thread modes.
The new API therefore requires Workgroups local size to equal its compiled
reflection and reports `UINT32_MAX` in those three specialization fields.
Exact-thread regions report0,1,2. Mode mismatch, invalid local size, AIR maximum
workgroup violations and arithmetic overflow return an error. This deliberately
does not reinterpret a fixed whole-group module as an exact-thread module.
Hardware dimensional/occupancy limits must additionally be enforced by the
actual pipeline/submission backend; planning numbers is not hardware admission.

For4099 threads at nominal64x1x1, the actual planner returns a64-thread variant
for64 groups and a3-thread variant for one tail group at thread base4096. The
second push payload is `[4099,1,1,4096,0,0,64,0,0,65,1,1]`. The original104-byte
fixture was compiled for64x1x1 only. Changing NUM_THREAD to3 without compiling
the actual specialized ACO variant would be incorrect. No new native class or
tail shader was admitted by this planner patch.

Focused evidence is in `build/metal-dispatch-plan/report.json`: three Rust tests
including exhaustive coverage of480 small3D grids, plus ten C ABI cases using
real Apple AIR compilation, ownership after compiler-result free, actual tail
payloads, mode/size/overflow failure and fixed Workgroups specialization rules.
The bounded native C/C++ layout check is separate in
`native-header-report.json`; local Darwin-target Rust type checking is in
`darwin-check-report.json`. Those original checks were layout/type checks only.
The later `build/metal-pipeline-native-20261011b/report.json` now records actual
native linking and C ABI execution of the current dispatch and pipeline modules,
with all23 exports and source hashes matching the worktree. The Mac stage peaked
at135,016,925 bytes and was removed. No GPU executed a planner region in these
compiler tests.

## Concrete next implementation toward general Metal compute

1. **Retained pipeline product and compiler orchestration — implemented offline.**
   `userspace/metal/src/pipeline_product.rs` and the additive
   `include/hackynavi_pipeline.h` now build owned products from the live shader's
   real plan, invoke NIR/ACO for each distinct local size, and retain exact code,
   typed RSRC, resource access, reflection, original SPIR-V/entry and push data.
   All variants must pass before publication. Real Apple add_one and the
   separately authored affine_xor both compile at local64 and local3, with
   byte-for-byte standalone ACO comparisons and independent owner lifetimes.
   Explicit subset limits and remaining admission work are recorded in
   [PIPELINE.md](../userspace/metal/PIPELINE.md). This is not yet an Objective-C
   `newComputePipelineState` implementation or permission to submit user ISA.
2. **Native immutable pipeline/job admission — implemented for the bounded
   Gfx1100LeafBufferV1 class.** The separate v1.3 product interface and native
   class policy admit independently decoded copied bytes. File hashes and user JSON are consistency
   checks, not authority to execute arbitrary user ISA. Independent kernel
   validation enforces the admitted instruction/resource subset, immutable
   capture, budgets and actual retained mappings under native per-I/O scopes.
   Every real code variant and canonical region is captured before submission;
   only real completion, readback and retirement can publish success. Ambiguity
   retains exposed resources. Native boundary linking is complete in the c1cb8
   milestone above. Target execution remains a validation step, and wider
   shader semantics remain implementation work.
3. **Reusable Metal resource/command semantics.** v1.4 now preserves native
   VM/data storage after completed-job consumption. The C++ runtime retains
   actual buffers, serializes dispatch/I/O, reserves stable addresses and
   closes the native context after ambiguous failures. The initial Objective-C
   buffer uses this real private storage. Target execution, shared/managed CPU
   copies and synchronization, asynchronous scheduling and broader resource
   operations remain. SDK `MTLBuffer.h` documents managed dirty-range behavior;
   that API is not implemented by silently returning a detached heap pointer.
4. **Objective-C compute frontend.** Implement actual library/function/pipeline,
   buffer, queue, command-buffer and compute-encoder objects. Public SDK entry
   points are `MTLDevice.h:628,667,765,771,819`,
   `MTLComputeCommandEncoder.h:59,71,228,243`, and
   `MTLCommandBuffer.h:306,348,354-366`. Snapshot bound buffers/pipeline and
   dispatch state at encoding, retain all referenced objects, honor endEncoding
   and single commit, deliver scheduled/completed/error states and callbacks
   from actual submission/completion, and carry errors across asynchronous
   compiler/command methods. No CPU reference calculation may populate an output
   or turn a failed command into Completed.
5. **Real discovery, then graphics/presentation.** Connect that frontend to the
   measured accelerator service/bundle path and verify registryID/device
   identity through Apple's normal enumeration. Report only feature families
   and limits that the full required behavior satisfies. The reference's
   `supportsFamily` answers are not our capability evidence. Expand real
   compiler/resource/render support and IOSurface/display integration before
   claiming general Metal or WindowServer acceleration.

The next verification sequence is target fixed-fixture execution, actual
compiler-variant generation/admission, then a real Objective-C compute client
with multiple command buffers and retained buffers. Registration and feature
advertising must reflect those implemented contracts. A GPU appearing in a
device list is an additional integration check, not a substitute for output,
error, lifetime and completion tests.

## License and source-use boundaries

NVIDIA `plugin/NVMTLDevice.m:1-5` identifies PolyForm-Noncommercial-1.0.0;
its root `LICENSE` is the same license. This audit copied no plugin or kext
implementation. Those files are architectural observations only and must not
be relabeled MIT. NVIDIA's separate `NOTICE` and the retained translator license
identify metal2vulkan as LGPL-3.0-or-later; our existing vendor subset keeps its
source, notices and separate license as recorded in `userspace/metal/NOTICE.md`.
The new adapter invokes that real library and does not copy/relicense it.

Cobalt's root license is MIT, but individual imported components and their
notices still need checking. Its source includes placeholder-pipeline behavior:
`Navi48Device.m:4241-4245,5713-5721` can construct a placeholder and skip compute
dispatch. That behavior is explicitly excluded here. Likewise, no validator
stub, constant-output shader or CPU fallback was introduced. The saved Apple
SDK/runtime capture records API facts; no Apple binary, private-framework
implementation or SDK distribution is added to the project.
