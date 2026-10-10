# Navi31 compute workgroup capacity

The current admitted profile is **GFX1100/Navi31, wave32, WGP mode, zero LDS,
scratch, shared VGPRs and spills**. For one fixed compiled variant, its maximum
resident workgroup depends on the actual VGPR count. The existing 1,024-thread
architectural ceiling alone is insufficient. The new pure helper in
`include/hackynavi/gfx11_occupancy.hpp` derives register capacity only; it is
not an ISA admission or execution certificate.

## Pinned source derivation

Mesa is pinned at `35b085c4c067f198ce0b6437d94678e47b9c645f` and Linux at
`3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`, recorded in `.research/sources.json`.
The local sources, not inferred GPU marketing specifications, establish these
quantities:

| Quantity | Pinned source and local location |
|---|---|
| 16 wave slots per SIMD on GFX10.3+ | [Mesa `ac_gpu_info.c`, lines 249–250](https://gitlab.freedesktop.org/mesa/mesa/-/blob/35b085c4c067f198ce0b6437d94678e47b9c645f/src/amd/common/ac_gpu_info.c#L249), local `.research/mesa/src/amd/common/ac_gpu_info.c` |
| Fixed 108 SGPR allocation per wave on GFX10+ | Same file, lines 258–262: physical SGPR capacity is `108 * max_waves_per_simd`; min/max/granularity are 108 |
| Navi31 has 768 wave64-equivalent VGPRs per SIMD | Same file, lines 290–292; this is the explicit Navi31 fallback also used by our offline target |
| Wave64 allocation granularity is physical VGPR capacity / 64; encoding granularity is four | Same file, lines 299–301 |
| Wave32 doubles physical VGPR capacity and allocation granularity | [Mesa `aco_ir.cpp`, lines 95–97](https://gitlab.freedesktop.org/mesa/mesa/-/blob/35b085c4c067f198ce0b6437d94678e47b9c645f/src/amd/compiler/aco_ir.cpp#L95), local `.research/mesa/src/amd/compiler/aco_ir.cpp`: hence 1,536 VGPRs and granularity 24 |
| Two SIMDs per CU on GFX10+ | `ac_gpu_info.c`, line 312; Linux agrees in `.research/linux/gfx_v11_0.c:7398` and `.research/linux/navi10_enum.h:433` (`NUM_SIMD_PER_CU = 2`) |
| A WGP contains twice the CU SIMD count | [Mesa `aco_live_var_analysis.cpp`, lines 625–630](https://gitlab.freedesktop.org/mesa/mesa/-/blob/35b085c4c067f198ce0b6437d94678e47b9c645f/src/amd/compiler/aco_live_var_analysis.cpp#L625), local `.research/mesa/src/amd/compiler/aco_live_var_analysis.cpp`: required waves per SIMD are ceil(workgroup waves / 4) in WGP mode |
| Round encoded wave32 VGPRs to eight, then real physical allocation to 24 | [Mesa `radv_shader.c`, lines 3013–3020](https://gitlab.freedesktop.org/mesa/mesa/-/blob/35b085c4c067f198ce0b6437d94678e47b9c645f/src/amd/vulkan/radv_shader.c#L3013), local `.research/mesa/src/amd/vulkan/radv_shader.c`; its GFX10+ occupancy does not apply the older variable-SGPR division |

The Linux enum comes from the exact pinned
[header](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/include/navi10_enum.h#L433),
whose download provenance is in `.research/linux/navi10_enum.h.source.json`.
Linux's SIMD count corroborates topology; Mesa supplies the register allocation
and workgroup occupancy rules. Neither source establishes target execution.

The current adapter explicitly selects `.family = CHIP_NAVI31`, GFX11 and
`.wgp_mode = true` in `userspace/compiler/compiler.c:365`; emitted metadata
records actual VGPR/SGPR allocation and WGP mode. The native ISA validator
derives `RSRC1 = 0x202c0000 | ((numVgprs-1)/8)`, including WGP bit `0x20000000`,
in `src/gfx11_compute_product.cpp`'s `validate` function. No caller-controlled CU-mode switch
is part of that admitted profile.

ACO's allocator publishes `min(get_vgpr_alloc(...), 256)` in
`.research/mesa/src/amd/compiler/aco_register_allocation.cpp:4323`. Therefore
published 256 is possible even though physical wave32 allocation rounds it
to **264**, and must not be divided directly into the 1,536-register budget.

## Formula and boundary values

For actual admitted `numVgprs = N`, with `1 <= N <= 256`:

```text
encodedVGPRs   = ceil(N / 8) * 8
allocatedVGPRs = ceil(encodedVGPRs / 24) * 24
wavesPerSIMD   = min(16, floor(1536 / allocatedVGPRs))
maxThreads    = min(1024, wavesPerSIMD * 4 * 32)
```

A launch with checked local-dimension product `T` is resident only when
`1 <= T <= maxThreads`. Equivalently, the required occupancy
`ceil(ceil(T / 32) / 4)` must not exceed `wavesPerSIMD`. The four-SIMD factor
requires WGP mode; applying it to a CU-mode launch would overstate capacity.

| Declared VGPR count | Physical allocation near limiting boundary | Maximum threads |
|---|---:|---:|
| 1–192 | Up to 192 | 1,024 |
| 193–216 | 216 | 896 |
| 217–240 | 240 | 768 |
| 241–256 | 264 | 640 |

The current real Apple `add_one` fixture reports 108 SGPRs and 24 VGPRs in
`userspace/compiler/fixtures/apple-add-one/shader.json`; its register capacity
is 1,024 threads. That does **not** change its compiled local dimensions or
prove a different dispatch shape can reuse its code. The helper also does not
replace the separate declared SGPR count and instruction-register checks.

## Metal property requirements

For this exact supported profile:

- `threadExecutionWidth` is **32**, from the actual admitted wave32 contract.
- `staticThreadgroupMemoryLength` is **0**, only after actual compiler/profile
  validation rejects LDS. Dynamic threadgroup storage remains unsupported;
  this zero must not hide an unsupported shader's declared threadgroup memory.
- A retained fixed set of compiled variants uses the minimum register capacity
  across **every** variant, further capped by the actual AIR
  `max_work_group_size` ceiling and any nonzero descriptor limit.
- A pipeline that compiles fresh local-size variants at dispatch cannot infer
  a global bound from one low-register probe. **640** is a conservative bound
  for every admitted VGPR count in the current profile. Its advertised limit
  must still be reduced by the actual AIR ceiling/descriptor limit, and each
  future variant must pass its own compiler, fixed-dimension and ISA checks.

The actual AIR ceiling is retained in
`userspace/metal/vendor/metal2vulkan/src/reflect/mod.rs:1371` and enforced by
`userspace/metal/src/dispatch_plan.rs:38`. Missing AIR ceiling is different
from an invalid zero ceiling; user descriptor zero means unspecified according
to the actual captured SDK header. The SDK declarations are in
`build/metal-discovery-prerequisites/sdk-headers/MTLComputePipeline.h`, at
`maxTotalThreadsPerThreadgroup`, `threadExecutionWidth` and
`staticThreadgroupMemoryLength`. A property value is not evidence that a
pipeline was created through normal Metal discovery or ran on the target.

Required inputs to integration are actual owned variant VGPR counts, checked
local products, the fixed wave32/WGP/no-LDS/no-scratch/no-shared-register
profile, actual AIR ceiling and descriptor cap. None may come only from
untrusted container stage/name metadata. The helper is now integrated into
`gfx11product::validate`: after checking dimensions, wave size and register
counts, it rejects a nonresident local product with `InvalidLaunch` before
instruction decoding or publication of derived access masks and RSRC fields.
The existing failure path clears those output fields. Native product capture
and dispatch revalidation therefore consume the same occupancy check.

This does not change the userspace wire layout or `Pipeline::capture`'s
frontend checks. The Objective-C compute-pipeline property layer is separate
work; its future dispatch variants still require actual ISA admission.

## Tests

`tests/gfx11_occupancy_tests.cpp` tests exact register-tier boundaries, partial
waves just above each maximum, all valid VGPR counts, invalid counts and
unchanged output on error. Its independent residency oracle distributes waves
round-robin into four SIMD register files and checks both physical-register and
wave-slot budgets for every 1–1,025-thread workgroup. These are arithmetic
boundary tests, not synthetic GPU completions or hardware execution claims.

`tests/gfx11_compute_product_tests.cpp` also uses the existing real ACO
`add_one` and `affine_xor` instruction images with varied declared VGPR
reservations. It checks every limiting tier at, below and above capacity,
all three dimension axes, multidimensional products, and cleared output on
rejection. These envelope tests do not claim that a fixed fixture was
recompiled for every tested local shape.

## Verified integration, 2026-10-11

| Evidence | Result |
|---|---|
| `build/gfx11-occupancy-local/report.json` | Helper strict and UBSan runs each passed 264,599 checks; freestanding x86_64 macOS header compilation passed. |
| `build/gfx11-occupancy-local/independent-review.json` | Source-derived helper review passed; SHA-256 `0c30ddb417c3ac74543938a346ab2fb34d981d86c5e5bca1107acd1a8b9f1183`. |
| `build/gfx11-occupancy-admission-local/report.json` | Integrated ISA suite passed 85,839 strict and 85,839 UBSan checks. Both targeted CMake tests passed. Only the ISA library and helper test require C++14; the project default remains C++11. |
| `build/native-gfx11-occupancy-admission/report.json` | Actual Apple ASan/UBSan passed 85,839 ISA, 264,599 occupancy and 120,732 dispatch-product checks, zero failures. One changed kernel translation unit compiled against the real SDK with the 2 KiB frame warning. |

The native report SHA-256 is
`2e0733358d23b65acc830aafaa064cb7e6d0c53f8740f918232b751964224534`.
Its ten source files total 95,768 bytes, match the verified archive and
worktree, and produced kernel object SHA-256
`68d9d7f1c12fe4544a827eb76986f2d4a7ac2f904dfc33f9254e42b6a282caea`.
The exact temporary Mac stage was removed after the native command completed.

This focused check did not link a new complete kext, load it, submit a GPU
command or instantiate a Metal device. The prior full kext artifact predates
this occupancy change; its earlier link evidence must not be attributed to
the changed validator.
