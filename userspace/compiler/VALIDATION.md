# Recorded compiler validation

This record describes the real offline compute compiler and its native C interface. It does not establish Metal device registration, a bootable accelerator, or execution of the generated ACO binary on the GPU.

The input is the genuine Apple-generated `add_one.air` fixture translated by `userspace/metal` and validated with real SPIRV-Tools 2025.2 for Vulkan 1.2. Its SPIR-V SHA-256 is `dc617a376ba9594f26dfb3b07656ef4a87236c5fb1ff387b02d0fdb8a59e2439`. Mesa is pinned to `35b085c4c067f198ce0b6437d94678e47b9c645f`; all 3,718 original source-manifest files were checked with zero hash mismatches.

The Windows CLI tests passed compilation of the fixture, local sizes 3×1×1, 8×8×1, and 4×4×4, changed arithmetic producing changed machine code, descriptor generation/bounds, and rejection of malformed input, missing validation, wrong entry, invalid local size, unsupported capability, binding, descriptor set, specialization, and a valid shader containing an unsupported workgroup barrier. The DLL tests passed actual compilation, identical output/configuration to the CLI, descriptor bounds, null arguments, sixteen wrong-entry failures without output, and successful deterministic compilation after failures. The final link has no undefined-symbol warning.

Both the native x86_64 executable and dylib passed on macOS 15.8 build 24H22. Their actual compilation results match the Windows machine code byte for byte and match every launch-metadata field. The native validator rejected the invalid-module challenge. The native C interface passed the same descriptor/null/repeated-failure/recovery checks. The dylib depends only on `/usr/lib/libSystem.B.dylib`. The private Mac stage peaked at 27,675,180 bytes and was removed; the final report measured 35,371,245,568 bytes free afterward. No toolchain was installed on the Mac and no GPU work was submitted.

Final verified artifacts:

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| Windows executable | 13,889,536 | `0494c73ae8508d97f2246226fd885181950ff1ea4804e8e0861955ad26f1646c` |
| Windows DLL | 13,886,976 | `3e2c61404677d7f33ae3a6c4e30b247e775353175f08d5f4d31cc8dc6a8037dc` |
| macOS executable | 12,514,412 | `43b1462cbaef967818d4702f5a455ecbd54b58841a7869afdd629ad6aa31667a` |
| macOS dylib | 12,514,225 | `c8b0176b98e263dbb09c6076b712c01ffda47adb62799d4f311fd55368ae0769` |
| Generated GFX11 code | 104 | `ac333acf99387a4063fae75a89dd4e6968907f6b78702d26442dd8a3575f7dda` |

The verified baseline has 84 executable bytes, wave32, local size 64×1×1, 24 VGPRs, 108 SGPRs, and no scratch, LDS, spills, constants, or relocations. The emitted register values are RSRC1 `539754498`, RSRC2 `1928`, RSRC3 `0`, and required wave32 dispatch bit `32768`. These values describe the explicit offline `gfx1100` target; they are not measured board capabilities. The original Metal dispatch metadata and the resource/launch contract in `README.md` must accompany the binary.

Evidence is retained under `build/gfx11-compiler`: `build-report.json`, `test-report.json`, `windows-c-abi.json`, `darwin-build-report.json`, and `macos-native/report.json`. Copies of the verified native executable and dylib are in `macos-native`. Rebuilds may change host-binary hashes; rerun the tests and use their new reports instead of treating this record as a certificate for changed files.
