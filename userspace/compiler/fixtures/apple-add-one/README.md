# AppleAddOneGfx1100V1

This is the exact retained compiler product from
`build/gfx11-compiler/apple-add-one-20261010-195643`. The authored MIT Metal kernel
was compiled to AIR by Apple's runtime compiler, translated to SPIR-V by the
separately licensed metal2vulkan adapter, validated by SPIRV-Tools, then compiled
by the HackyNavi Mesa NIR/ACO adapter for its explicit offline gfx1100 target.
All 104 machine-code bytes are preserved, including the 20 bytes after the
84-byte executable region. This ACO binary has no recorded GPU execution.
Earlier Windows Vulkan tests compiled their own machine code from SPIR-V and
do not establish execution of this binary.

From the repository root:

```text
python userspace/compiler/verify_fixture.py --check
python userspace/compiler/verify_fixture.py --self-test
python userspace/compiler/verify_fixture.py --write-header
```

Verification requires no compiler installation, network, Mac, or GPU. The
manifest records every packaged file's exact byte count, SHA-256, original path,
role, and license context. The verifier pins the manifest's own digest and checks
the metadata/reflection/source chain. Updating a payload and its manifest hash
does not admit a replacement. A different shader or launch contract requires a
reviewed product version and verifier update. `--write-header` only reproduces
the fixed byte array after verifying its inputs; failure publishes no header.

`shader_data.hpp` exposes `hackynavi::gfxaco::fixture::kCode`, `kCodeBytes`,
`kExecutableBytes`, and `kCodeAlignment` as C++11 `static constexpr` data.
The generated header contains only the original code bytes. The native loader
owns mapping, page-tail initialization, cache synchronization and completion.

The launch contract is fixed: wave32, local size 64x1x1, set 0 buffer bindings 0
(read) and 1 (write), 16-byte descriptors, and a 48-byte `ThreadsDynamic` payload
at offset zero. SGPRs 0-1 hold the full descriptor-table VA; SGPRs 2-3 hold the
full push-constant VA. These are the HackyNavi compiler adapter's arguments,
not RADV's ordinary 32-bit descriptor-set pointers or an HSA kernarg layout.
Preserve the recorded RSRC values and system inputs from `shader.json`. This
fixed-local binary does not implement arbitrary tail-group specialization.

Tool provenance distinguishes historical recorded hashes from binaries merely
observed during packaging. The translation invocation did not record a unique
translator executable hash; the manifest says so. Historical command captures
are evidence, not setup instructions. License notices and source manifests are
retained under `licenses/` and `provenance/`; no NVIDIA PolyForm frontend/kext,
Apple system library, compiler executable, or GPU firmware is copied here.
