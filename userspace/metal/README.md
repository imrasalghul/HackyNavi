# HackyNavi AIR shader compiler

This is a real AIR/LLVM-to-Vulkan-SPIR-V compiler component for the future Metal
userspace driver. It builds a Rust library, a C ABI dynamic library, and a CLI.
It does **not** register an `MTLDevice`, implement a macOS GPU user client, submit
commands through HackyNavi, or enable WindowServer acceleration.

Hardware evidence includes both authored LLVM IR and actual Apple-generated AIR
for our add-one MSL kernel, translated to SPIR-V and executed on the physical
RX 7900 XTX through its installed Windows Vulkan driver. Each input passed
4,096 whole-workgroup results and 4,099 exact-thread results, with 64 untouched
guard words beyond each grid. This proves these compiler/dispatch cases, not
macOS Metal or HackyNavi engine operation.

The additive [owned pipeline API](PIPELINE.md) now invokes real Mesa NIR/ACO
for every local-size variant and retains machine code, resource access and
launch metadata. Its current native library passes compiler, dispatch and
pipeline C ABI tests with all 23 exports. Native code admission and the Metal
frontend remain separate from this offline compiler component.

## Build and test

Requirements: Rust 1.90+, Python 3.10+, LLVM `llvm-as`/`llvm-dis`, and Khronos
SPIRV-Tools `spirv-val`. The Windows workspace has local Rust 1.90.0, LLVM 20.1.8,
and SPIRV-Tools v2025.2 under `.tools/userspace`.

```powershell
python userspace/metal/test-local.py
python userspace/metal/test-local.py --vulkan-test
```

The second command requires a real `1002:744c` discrete Vulkan device and fails
if it is absent. It never substitutes another GPU or software rendering. The
hardware tests only use the installed OS Vulkan driver.

On another host, provide actual native executable paths:

```sh
python3 userspace/metal/test-local.py \
  --cargo /path/to/cargo --llvm-as /path/to/llvm-as \
  --llvm-dis /path/to/llvm-dis --spirv-val /path/to/spirv-val
```

The script builds with `--locked`, writes only to `build/metal-compiler`, and
records command exits, tool versions/hashes, source hashes, and test output in
`test-report.json` and `test.log`. It installs nothing and does not connect to
the Mac. The optional Vulkan dependency is absent from the ordinary compiler
build. The compiler C ABI also runs on Intel macOS 15.8: Rust objects were
cross-compiled on Windows, linked into an x86_64 dylib using the Mac's existing
Apple Clang, and loaded/tested on the Mac with a real macOS SPIRV-Tools validator.

The local SPIRV-Tools build uses its v2025.2 source archive (SHA-256
`4a6147f9cb00ca33adedeb717c4b397480cd7f7f3128ac24dee8c24b6145ecf3`) and
SPIRV-Headers commit `aa6cef192b8e693916eb713e7a9ccadf06062ceb` (archive SHA-256
`1bf0568bad1f9e30fce8d3199e54ae7ade1c16dbe913b5afd9b499149a9bb9a5`). The
validator's build description explicitly records the source archive instead of
mistakenly identifying HackyNavi's enclosing Git commit as SPIRV-Tools' revision.

## Compiler contract

`compile(Request)` accepts raw LLVM bitcode, Apple's LLVM bitcode wrapper, or
textual LLVM IR with AIR metadata. It requires a stage and exact Metal entry
name. AIR bitcode is decoded by `llvm-dis`. A translator-emitted shader must
pass `spirv-val --target-env vulkan1.2`, descriptor-reflection validation, and an
exact stage/entry check before its words can be returned. Every invocation also
checks the real validator's identity and that it rejects an invalid SPIR-V
module. Missing tools and unsupported operations return errors.

The fixture in `tests/fixtures/add_one.ll` is authored source, not a captured
Apple shader. Tests assemble it into genuine LLVM bitcode and verify that
bitcode and text compile to the same shader. They also check storage load/add/
store instructions, descriptor locations, C result ownership, unsupported
intrinsics, missing validation, wrong entry selection, and invalid arguments.

`tests/fixtures/apple/add_one.air` is genuine Apple compiler output for the
tracked `add_one.metal` source, captured from the mini's existing Metal runtime
compiler (Apple metal 32023.622, AIR 2.7, MSL 2.4, macOS 15.8). Its provenance
records hashes, actual commands, the original 24,035-byte temporary footprint,
and successful cleanup. Both authored IR and Apple AIR have physical Navi31
execution tests.
Regenerate evidence without installing a Metal toolchain using:

```powershell
python userspace/metal/capture-apple-fixture.py --host victor@mini
```

The capture tries existing `xcrun metal` first. If the optional toolchain is
absent, a small helper uses `newLibraryWithSource` and checks for the private
`MTLFunction.bitcodeData` selector. An unavailable selector returns an explicit
error; no substitute AIR is created. Captured output goes under
`build/metal-compiler/apple-input`, not over the committed fixture.
The script also builds/tests the small `apple-air-dis` helper against the
existing system LLVM and returns its actual decoded IR and executable.
The completed reader validation used 43,500 bytes of temporary storage and
verified malformed-bitcode and unavailable-library failures. Its decoded IR
produced byte-identical SPIR-V to LLVM 20's decoding of the same Apple AIR.

ABI v1 **rejects all Metal function-constant declarations/references**, because
it does not yet accept `MTLFunctionConstantValues`. In particular, it rejects
partial-effect kernels where silently defaulting a constant could erase one
write while leaving other writes intact. Specialization must be implemented
with actual supplied values before lifting that restriction.

`KernelDispatch::Workgroups` is for `dispatchThreadgroups` only. Exact-thread
dispatch uses the reflection's `kernel_dispatch.plan(...)`; the backend must
create pipeline variants with local-size specialization IDs `[0,1,2]`, submit
every returned region, and bind each region's 48-byte push-constant payload.
The hardware test exercises a 3-thread tail after 64 full 64-thread groups.
The C header documents this ABI; the C consumer's Metal encoder is still to be
implemented. Vertex and fragment inputs reach the upstream compiler API but
have not yet been covered by HackyNavi hardware tests. No broad Metal feature
or shader-corpus conformance claim follows from these compute tests.

The actual SPIR-V execution entry name is exposed separately from the Metal
name: `CompiledShader.spirv_entry_point` / `hn_shader_result_spirv_entry_point`.
Use it for `VkPipelineShaderStageCreateInfo::pName`. C callers borrow the aligned
word array and reflection string until `hn_shader_result_free`; errors return
zero words and explicit diagnostics. Configure tool environment overrides
before starting compiler threads; the library does not mutate process state.

The CLI takes an existing input, exact entry, stage, and a new output directory:

```sh
hackynavi-shader add_one.air add_one compute new-output-directory
```

Set `METAL2VULKAN_LLVM_DIS` and `METAL2VULKAN_SPIRV_VAL` to genuine tool paths or
make those tools available on PATH. Success writes `shader.spv`, `module.json`,
and `reflection.json`; a failed compilation publishes no result directory.
This entry point currently uses 64x1x1 nominal compute groups and exact-thread
dispatch. Consumers need to honor the returned dispatch contract.

## Storage-conscious native Mac validation

The mini currently lacks Rust, `llvm-dis`, and `spirv-val`; its `metal` executable
is a shim for an uninstalled Xcode component. None were installed. Instead, the
Windows Rust toolchain builds an x86_64 Darwin static library, and the existing
Apple Clang links the final dylib on the Mac. A genuine cross-built macOS
SPIRV-Tools validator accompanies the test. The dylib has an `@rpath` install name.

```powershell
./userspace/metal/build-validator.ps1 -Target MacOS
python userspace/metal/build-darwin-objects.py
python userspace/metal/test-macos-staged.py --host victor@mini
```

The first command requires the workspace Rust `x86_64-apple-darwin` standard
library target. The second requires the local macOS validator build at
`.tools/userspace/build-spirv-tools-macos/tools/spirv-val`. It checks available
space, source/object hashes and archive membership, and uploads 20.1 MB compressed.
The successful native run peaked at 121.3 MB in a unique temporary directory,
then removed it. It returned the dylib and full evidence to
`build/metal-compiler/macos-native`. The native tests verified both authored IR
and the Apple-generated shader's decoded LLVM IR and raw AIR bytes, with
successful and explicitly rejected operations. The native `apple-air-dis`
helper supplies real bitcode decoding through the system GPUCompiler
framework's LLVM C API. It resolves libraries through `dlopen`, including dyld
shared-cache libraries without on-disk files, and checks every required symbol.
It returns an explicit error if the private framework/API is unavailable; it
does not fabricate IR. Set `METAL2VULKAN_LLVM_DIS` to this helper on macOS.

This is native linking and runtime validation of cross-compiled Rust objects,
not a claim that a Rust toolchain ran on the mini. It does not install, load, or
modify a kext, OpenCore, or a system Metal driver.

## Work still required for Metal

- Metal library/metallib parsing and exact function selection. Driver-side Metal
  Shading Language compilation remains unimplemented; the fixture capture helper
  requires an existing system Metal device.
- Actual function-constant, sampler, image, threadgroup-memory and other pipeline
  specialization inputs, with semantic tests for each supported contract.
- A Metal Objective-C device/resource/pipeline/command implementation connected
  to RADV/ACO for Navi31, with explicit unsupported-feature errors.
- Reusable resource/command semantics beyond the implemented native one-job
  user client, VM mapping, queue completion/readback and retirement. The v1.3
  boundary admits actual compiler products; its target-only tool still needs
  hardware execution evidence. See [integration status](../../docs/validation-product-wire-power.md).
- IOSurface/framebuffer presentation and target hardware execution evidence
  through HackyNavi.

Licensing and the pinned source manifest are described in [NOTICE.md](NOTICE.md).
