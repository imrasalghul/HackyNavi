# Experimental native Metal bundle packaging

`package-bundle.py` creates a local, reproducible development archive from
successful native build reports and their exact immutable artifacts. It does
not install, sign, load or execute the plugin, change system policy, instantiate
a Metal device or submit GPU work.

The measured candidate installation path is
`/Library/GPUBundles/HackyNaviMetal.bundle`. The actual accelerator publishes
`MetalPluginName=HackyNaviMetal` and `MetalPluginClassName=HNMetalDevice`.
`Contents/Info.plist` sets `NSPrincipalClass=HNMetalDevice` and
`CFBundleExecutable=HackyNaviMetal`. The complete layout is:

```text
HackyNaviMetal.bundle/
  Contents/
    Info.plist
    MacOS/HackyNaviMetal
    Frameworks/libhackynavi_metal_compiler.dylib
    Frameworks/libHackyNaviGraphicsCompiler.dylib  # graphics profile only
    Frameworks/libHackyNaviColorLayout.dylib       # graphics profile only
    Helpers/apple-air-dis
    Helpers/spirv-val
    Helpers/hackynavi-aco
    Helpers/hackynavi-aco-ngg                     # graphics profile only
    Helpers/hackynavi-aco-fragment                # graphics profile only
    Helpers/hackynavi-aco-fragment-buffer         # graphics profile only
    Helpers/hackynavi-aco-vertex-buffer           # graphics profile only
    Resources/Licenses/
    Resources/Provenance/
    Resources/Sources/HackyNaviMetal-source.tar.gz
    Resources/Tests/                             # graphics profile only
    Resources/DEVELOPMENT.txt
```

The main executable is a real x86_64 `MH_BUNDLE`, linked on the Mac from the
actual frontend objects. The compute profile requires
`@rpath/libhackynavi_metal_compiler.dylib`; the graphics profile additionally
requires the exact two graphics libraries shown above, also through `@rpath`.
The only allowed rpath is
`@loader_path/../Frameworks`. All other direct dependencies must reside in
system framework/library paths. The compiler's actual 32 dynamic exports and
install name are checked. Each helper must be a real x86_64 executable with
only system dependencies and no rpaths. No Mach-O byte patching occurs.
The graphics compiler must export exactly its four validated-file C functions
and actual fragment-buffer and vertex-buffer descriptor oracles;
the layout library must export exactly calculate/validate/validate-inputs.
Each has its own checked install name and only system dependencies. A graphics
frontend cannot be packaged under the compute-only dependency policy, and
missing, swapped or untested graphics libraries reject before output is written.
Every image must declare a native macOS target with a nonzero compatible
minimum version no later than 15.0. The parser accepts platform 1 in
`LC_BUILD_VERSION` with its exact bounded tool table, or the fixed legacy
`LC_VERSION_MIN_MACOSX` command. Missing, duplicate and non-macOS targets reject.

The bundled AIR helper is separately rebuilt from source without the former
`HACKYNAVI_APPLE_LLVM_LIBRARY` override. It tries only fixed system GPUCompiler
and MTLCompiler paths. Apple LLVM is supplied by macOS and is not redistributed.
Actual valid/invalid AIR tests verify that a bogus override cannot redirect the
helper. The helper is also exercised through the existing compiler's 94 real
explicit-tools checks, including retained tools and full/tail ACO compilation.
The compiler itself is reused byte-for-byte from its successful native build.

## Build the local package

```powershell
C:/Python314/python.exe userspace/metal/package-bundle.py `
  --frontend-report build/metal-device-native-2/report.json `
  --compiler-report build/metal-explicit-tools-native-final-2/report.json `
  --reader-report build/metal-system-reader-native-final/report.json `
  --aco-report build/gfx11-compiler/darwin-build-report.json `
  --output build/metal-bundle-package-final-3
```

Choose a fresh output directory. The script verifies every staged input member,
the matching compiler used for the actual frontend/helper tests, each copied
binary's size/hash, direct load commands and exported class identity. The
package report contains a hash and archive mode for every file. The tar archive
records executable/helper/dylib mode 0755 and data mode 0644, including when it
is created on Windows. It records directory mode 0755 and contains no symlinks,
absolute paths or source filesystem owner/timestamp metadata.

For the graphics frontend, use the matching native reports:

```powershell
C:/Python314/python.exe userspace/metal/package-bundle.py `
  --frontend-report build/vertex-frontend-native-1/report.json `
  --compiler-report build/metal-explicit-tools-native-final-2/report.json `
  --reader-report build/metal-system-reader-native-final/report.json `
  --aco-report build/gfx11-compiler/darwin-build-report.json `
  --graphics-report build/graphics-vertex-buffer-darwin-native-1/report.json `
  --graphics-runtime-report build/graphics-vertex-runtime-native-1/report.json `
  --output build/metal-vertex-development-package-next
```

Both graphics arguments are mandatory when the actual frontend report declares
graphics. The frontend snapshot must contain the identical report and library
bytes. The native graphics report proves 270 real compiler checks/16 actual
Apple AIR/metallib products and 249,778 layout checks/493 Mesa products. The
runtime report adds 96 checks/eight actual source-to-pipeline products with
ASan/UBSan on the six authored C++ translation units. Reused compiler libraries
were not sanitizer-instrumented. Runtime source and dependencies must match the
frontend snapshot, and the actual compiler, fixed AIR reader and SPIR-V validator
must match the packaged bytes. None of these tests opens a device or executes
GPU work.

The graphics archive also carries the actual compiled
`hackynavi-metal-compute-smoke`, `hackynavi-metal-render-smoke`,
`hackynavi-metal-render-buffer-smoke` and `hackynavi-metal-render-vertex-buffer-smoke` under
`Contents/Resources/Tests`, outside plugin/helper discovery. These binaries come
from the exact frontend report. The directory contains the verified Apple
`graphics.metallib`, `fragment-buffer.metallib`, `vertex-buffer.metallib` and
`add_one.metallib`, original fixture provenance,
instructions and a separate file/hash/mode manifest. These programs are compiled
but have not been run on the target. Copy that directory to a writable user
directory and follow its README only after the matching target driver is loaded
and the frontend is discoverable. All require an explicit `--run` and the
actual accelerator registry ID; they do not select a fallback GPU. The packager
does not execute them or establish target readiness.

Licenses and notices retain their original component identities. The added
SHA-256 dependency licenses are copied from Cargo source archives whose hashes
match the lockfile. The source archive includes the patched LGPL translator,
adapter/runtime sources, actual build/test scripts, complete locked Cargo
source archives, and the pinned materialized Mesa source slice with its
individual copyright notices. Rebuild toolchains and Apple frameworks remain
external prerequisites. The compiler dynamic library remains separately
replaceable; the package contains no signing key or code-signing operation.
Rust standard-library copyright/license notices and the available Zig/LLVM
C++ runtime license texts are retained alongside the component licenses.
Graphics packages also retain the native relink commands, exact archive/member
identities, authored adapter sources, generated layout-only `ac_surface` copy,
actual included Mesa/generated headers, runtime harness/fixtures and graphics
validation reports. The graphics libraries use the matching private Zig C++
runtime, retaining its notices. The build reused real Darwin Mesa archives;
it did not independently reconstruct every old archive from source, and the
package states that limit. The pinned Mesa source slice remains included.

## What this verifies

Packaging establishes file layout, binary identity, direct dependency closure
and source/license provenance. The archive is still experimental: NSBundle/dyld
loading, application library-validation policy, target loader trust, real
device discovery and GPU execution require separate target tests. The measured
15.8 loader policy is documented in `docs/metal-bundle-trust.md`; this packager
does not assume the user's 15.8.1 policy or modify it. Rendering/display support
and general Metal application compatibility are not established by this build.
