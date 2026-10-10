# Shader compiler provenance

HackyNavi's adapter, C interface, fixtures, and tests in this directory are MIT
licensed under the repository's `LICENSE`.

`vendor/metal2vulkan` is an **unmodified** source subset of the metal2vulkan
translator attributed upstream to Anees Iqbal and contributors. It is copied
from `translator/translator` in
<https://github.com/nullmoth/nvidia-macos-driver> at commit
`508f3d2496e78a9640d59128a56ddb5a53061606`. The translator's package metadata
identifies its original repository as <https://github.com/steelbrain/metal2vulkan>.
The retained subset contains the complete library/CLI sources, Cargo manifest
and upstream lockfile, README, and both LGPLv3 `LICENSE` and GPLv3 `COPYING` texts.
It does not contain the NVIDIA driver's PolyForm-licensed frontend or kext.

The translator is **LGPL-3.0-or-later**. It remains separately identified and
rebuildable from source. The root MIT license does not replace its license.
`vendor/source-manifest.json` records the upstream paths and SHA-256 of every
retained file. No patches have been applied to those files. The adapter's Cargo
lockfile separately pins the dependencies actually used for this build.

The compiler requires LLVM's real `llvm-dis` for AIR bitcode and Khronos
SPIRV-Tools' real `spirv-val`. They are external tools, not vendored here.
No Cobalt64 validator stub, constant-color shader, or no-op compute shader is
included. The optional hardware test uses `ash` to call the installed Vulkan
driver and explicitly identifies this separately from HackyNavi execution.

`tools/apple_air_dis.c` is new MIT code using LLVM's C API and the system-library
discovery approach documented by Cobalt64's `n48_xlate.h` at commit
`d6c9aedf8e6526a39e349b5886cbc8c7ddf77f65` in
<https://github.com/Almosst-DEV/Cobalt64>. The actual Apple library is supplied by
macOS and is not copied into this repository. The reader is a separate process;
the Rust compiler still requires and runs genuine SPIRV-Tools validation.

The `licenses` directory retains the license/notice files of the resolved Cargo
dependencies used in the compiler and optional hardware test. Package versions,
SPDX expressions, and file hashes are in `licenses/manifest.json`.

