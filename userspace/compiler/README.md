# Navi 31 offline shader compiler

This component compiles validated Vulkan SPIR-V compute shaders into real GFX11 machine code with Mesa NIR and ACO. It has compiled the actual Apple-generated `add_one.air` fixture after translation by `userspace/metal`. Both its executable and C library have run successfully on Windows and x86_64 macOS 15.8, producing identical shader code and launch metadata. It does not create a Vulkan device, submit GPU work, provide an `MTLDevice`, or make macOS acceleration available.

The verified fixture produces 104 code bytes, including 84 executable bytes, for `gfx1100`, wave32, with 24 allocated VGPRs, 108 allocated SGPRs, no scratch, no LDS, and no relocations. Its ACO IR retains the buffer load, increment, and buffer store. Changing the increment changes the machine code. This ACO binary has **not** been executed on the GPU. The earlier hardware tests used the same input SPIR-V with the AMD Windows Vulkan driver, which compiled its own binary.

## Build and test

The Mesa pin is `35b085c4c067f198ce0b6437d94678e47b9c645f`. `build.py` refuses another revision or tracked modifications, stages sources under `.tools/userspace/mesa-compiler-source`, and builds only the NIR/ACO dependency slice. The original Mesa checkout remains unchanged apart from sparse-checkout materialization. No Mesa community changes are submitted. No Asahi sources are edited or built.

Prerequisites already used locally:

- Python 3.14 and local Python packages Meson 1.9.1, Mako 1.3.10, PyYAML 6.0.3, packaging 25.0, MarkupSafe 3.0.4 under `.tools/userspace/python-build`.
- Zig 0.17.0 at `.tools/zig-x86_64-windows-0.17.0/zig.exe` and Ninja 1.13.2 at `.tools/build-python/bin/ninja.exe`.
- The real SPIRV-Tools 2025.2 validator already built by `userspace/metal/build-validator.ps1`. The validator is mandatory, identifies itself, and must reject a deliberately invalid module before the input is accepted.

Materialize the pinned dependency directories if the Mesa checkout is still sparse:

```powershell
git -C .research/mesa sparse-checkout add --skip-checks include bin subprojects licenses src/compiler src/util src/c11 src/gtest src/android_stub src/tool src/amd/addrlib src/mesa/main/extensions_table.c src/mesa/program/prog_parameter.c src/mesa/program/symbol_table.c src/mesa/program/dummy_errors.c src/gallium/include src/gallium/auxiliary src/gallium/frontends src/gallium/drivers/llvmpipe src/virtio src/vulkan/runtime/bvh
python -m pip install --target .tools/userspace/python-build meson==1.9.1 Mako==1.3.10 PyYAML==6.0.3 packaging==25.0 MarkupSafe==3.0.4
python userspace/compiler/build.py
python userspace/compiler/test.py
```

The build needs at least 4 GiB free local disk space. It installs nothing on the Mac. Mesa is configured with all GPU driver frontends and LLVM disabled. Its actual condition-variable implementation is selected because this Zig distribution lacks the Windows `synchronization` import library. The Windows stage selects real scalar addrlib code because this Zig runtime lacks the AVX2 feature-detection support symbol. `source-manifest.json` records original file hashes and staged compatibility changes; no replacement feature-query function is supplied.

`build/gfx11-compiler/build-report.json` records the executable/library hashes, source hashes, configure command, and Zig version. `test-report.json` records the checks and retained product directory. Tests include the actual Apple AIR-derived SPIR-V, changed arithmetic, local-size specializations, descriptor address/bounds generation, and rejection of unsupported inputs without publishing output. A valid SPIR-V shader containing a workgroup control barrier passes SPIRV-Tools and is rejected by the backend as unsupported.

```powershell
python userspace/compiler/compile.py build/metal-compiler/apple-input/translated/shader.spv build/gfx11-compiler/new-output --entry main --local 64 1 1
```

The output directory must be new. Output appears atomically only after validation, successful compilation, and consistency checks. The executable is an internal helper for `compile.py`; the helper itself is not the SPIR-V validation boundary.

`hackynavi_gfx11.h` exposes the same real compiler through a C ABI. `hn_gfx11_compile_validated_file` requires an input snapshot that has already passed real SPIRV-Tools validation for Vulkan 1.2. The caller supplies the entry, three local-size values, and an existing private temporary output directory. A zero return means all product files are complete; discard the temporary directory on any failure. The function retains no caller memory and submits no GPU work. `hn_gfx11_raw_buffer_descriptor` constructs the actual hardware descriptor with address/bounds checks. The Windows DLL has passed `check_c_abi.py`, including binary/metadata comparison with the CLI, repeated wrong-entry failures, null arguments, and successful compilation after failures.

## Native macOS compiler

```powershell
python userspace/compiler/build-darwin.py
python userspace/compiler/test-macos-staged.py --host victor@mini
```

The Darwin build cross-compiles locally for `x86_64-macos.15.0` with Zig's actual Darwin headers. It retains only AMD compiler helpers and NIR in AMD common and extracts the exact pinned `ac_fill_compiler_info` implementation; it does not invent board-query results or permit unresolved symbols. `HAVE_TSERVER` selects public addrlib typedefs in place of an Apple-private header. `source-manifest-darwin.json` records these changes and source hashes.

The staged test requires at least 1 GiB free on the Mac, uploads approximately 28 MB into its own uniquely named cache directory, runs the real executable and C library with the real native SPIRV-Tools validator, compares both against the Windows result, and removes the stage. It installs nothing and submits no GPU work. The successful test ran on macOS 15.8 build 24H22, x86_64, and confirmed a 27.7 MB peak stage and cleanup. The dylib depends only on system `libSystem.B.dylib`. Load the dylib by its resolved absolute path; packaging a linked frontend and its install-name policy remains integration work.

The executable and dylib are under `.tools/userspace/build-mesa-compiler-darwin/hackynavi-compiler`. `build/gfx11-compiler/darwin-build-report.json` records their hashes. `build/gfx11-compiler/macos-native/report.json` retains every native command, exit status, input hash, output comparison, platform version, disk use, and cleanup result. Native C ABI tests also cover descriptor bounds, null arguments, sixteen repeated wrong-entry failures without output, and successful deterministic compilation after failures.

## Launch contract

`shader.bin` contains little-endian code words. `shader.json` contains the explicit compiler target, workgroup size, wave size, code and executable sizes, ACO allocation/configuration, launch register values, resource mapping, and absence of unsupported relocations. `aco.txt` is ACO's recorded IR. `provenance.json` records tool and input/output hashes. The target profile is an offline compilation choice, not queried board information.

- Allocate executable GPU memory aligned to at least 256 bytes and preserve the complete code, including trailing ACO padding. The loader must provide its own code upload and cache synchronization.
- User SGPRs 0–1 contain the 64-bit GPU address of a descriptor table aligned to 16 bytes. Each set-0 binding uses one 16-byte GFX11 raw buffer descriptor at `binding * 16`. The table must remain resident, immutable, and accessible for the dispatch. Only bindings reported in the mask are read.
- User SGPRs 2–3 contain the 64-bit GPU address of the 48-byte Metal dispatch push-constant payload aligned to 4 bytes. The exact-thread region payload comes from the Metal translator's dispatch planner; this compiler does not invent those values.
- System SGPRs 4, 5, and 6 hold workgroup X, Y, and Z; SGPR 7 holds TG-size information. VGPR 0 holds GFX11's packed local invocation ID. The recorded RSRC2 enables those exact inputs.
- The launcher must use the recorded RSRC1/RSRC2/RSRC3, workgroup dimensions, and wave32 dispatch bit. RSRC3 instruction prefetch is disabled. Queue ownership, VMID/GPU mappings, dispatch origin, memory barriers, and completion synchronization remain the driver's responsibility.
- Specialization IDs 0, 1, and 2 are explicitly supplied from `--local`. The compiler checks the resolved NIR workgroup size matches. The corresponding original Metal dispatch metadata must travel with the product.

The internal helper can construct descriptors with Mesa's actual GFX11 descriptor builder:

```powershell
.tools/userspace/build-mesa-compiler/hackynavi-compiler/hackynavi-aco.exe --raw-buffer-descriptor 0x1234000000 16384
```

The result is four little-endian descriptor words; addresses are GPU virtual addresses, and sizes are bytes. It rejects zero addresses/sizes, a size exceeding 32 bits, and ranges beyond the 48-bit address boundary.

## Current supported boundary

The entry stage is compute. The resource ABI supports singleton storage buffers in set 0, bindings 0–31, and constant-offset 32-bit loads within a 48-byte push-constant payload. Only Shader and Int64 SPIR-V capabilities are accepted. Textures, shared-memory resources, descriptor arrays/reindexing, nonuniform descriptors, unsupported residual NIR intrinsics, unexpected specializations, scratch, LDS, constants, and relocations are rejected. General shader conformance is not established; the current evidence covers the stated compute fixtures and arithmetic/specialization variants.

The full Metal frontend, graphics stages, and integration with the HackyNavi compute transport remain separate work. LLVM 20.1.8's downloaded Windows `llvm-mc` build does not contain an AMDGPU target, so it cannot provide independent GFX11 disassembly; no disassembly success is claimed.

See `NOTICE.md` and `source-manifest.json` for source licensing and provenance.
