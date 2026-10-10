# Narrow GFX11 color-target prerequisite

The separate userspace calculator now invokes actual pinned Mesa/addrlib, and
the portable validator checks its exact narrow profile. This is not an observed
card layout, an allocation owner, a draw implementation or a rendering result.
The existing driver does not yet capture the address-library topology inputs
described below. No fallback topology is authorized by this implementation.

The first target is one non-scanout, two-dimensional
`PIPE_FORMAT_R8G8B8A8_UNORM` color image: one layer, mip level and sample, no
depth/stencil, DCC, CMASK, FMASK, sparse residency, imported memory or texture
access. Width and height are nonzero and at most 16384; the allocator's smaller
per-buffer and connection budgets remain independent admission limits. This
profile makes row-by-row CPU readback possible after actual graphics retirement.

## Actual source and required observations

Mesa is pinned to `35b085c4c067f198ce0b6437d94678e47b9c645f`; Linux is pinned to
`3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`. The relevant local source is unchanged.

| Input | Actual source | Capture requirement |
| --- | --- | --- |
| Navi 31 / GFX11.0.0 identity | Mesa `amd_family.h`, `amdgpu_asic_addr.h`; Linux `soc21.c:571` | Match the real PCI identity and parsed IP discovery, not a user-supplied family enum. |
| `GB_ADDR_CONFIG` | Linux `gfx_v11_0.c:4807`; `gc_11_0_0_offset.h:2390` | Read GC segment 0, register DWORD offset `0x13de`, through the validated IP layout/BAR. Linux rejects zero. |
| ASIC revision strap | Linux `nbio_v4_3.c:39`; `nbio_4_3_0_offset.h:620` | Read NBIO segment 2, register DWORD offset `0x16`; extract `(raw >> 24) & 0xf`. This is **not** PCI configuration revision. |
| External revision | Linux `soc21.c:596` | For actual GC 11.0.0, the pinned driver uses strap revision plus one. Mesa's Navi 31 range is `[1,16)`; reject a result outside it. |
| Memory/generation profile | Actual Navi 31 identity plus pinned Mesa | `FAMILY_NV3`, GFX11, dedicated VRAM and graphics/image-opcode support; derive generation flags through actual `ac_fill_compiler_info`. Do not populate arbitrary topology fields. |

GC/NBIO segment offsets are not absolute BAR byte addresses. Resolve them through
the real discovered layout with checked arithmetic. A future native capture must
hold the normal same-generation device authority and exclude reset/removal; its
complete repeated raw observations must agree. Retain the raw values with the
epoch. Read failure, all-ones data, unsupported IP identity, or a changed snapshot
must reject publication. No new MMIO read is performed by this proposal.

`ac_addrlib_create` (`ac_surface.c:1006`) passes the actual raw address config,
family and external revision to `AddrCreate`. GFX11 does not use the old
bank/rank/tile-array inputs. `gfx11addrlib.cpp:618` accepts pipe exponents 0..6,
requires the supported 256-byte interleave, and assumes packer exponent is no
greater than pipe exponent, with a difference at most two. Packer exponent
above five also exceeds the 75-entry D-swizzle-pattern tables initialized by
`AddrCreate`; reject it before calling the library. The adapter must
check these preconditions before calling addrlib: release-build assertions are
not input validation. Preserve the complete raw capture, without inventing the
values of unrelated register fields.

## Genuine calculation and bounded output

Use the real pinned `ac_addrlib_create`, `ac_compute_surface`,
`ac_init_cb_surface` and `ac_set_mutable_cb_surface_fields`. The surface config
sets block size 1x1, four bytes per pixel, one layer/mip/sample and
`RADEON_SURF_DISABLE_DCC | RADEON_SURF_NO_TEXTURE`, with an invalid modifier and
`RADEON_SURF_MODE_LINEAR_ALIGNED`. It does not set `NO_RENDER_TARGET`.

This is an allowed color profile, not an assumption based on compute buffers:
`Gfx11Lib::ValidateSwModeParams` accepts linear 2D color when it is neither a
depth buffer nor multisampled (`gfx11addrlib.cpp:2231..2357`). The actual linear
calculator uses 256-byte pitch/base alignment and a 64-bit slice-size product
(`gfx11addrlib.cpp:4640..4710`). The adapter must verify the actual output:
linear swizzle, two-dimensional resource, zero offsets/tile XOR/metadata,
unaltered image dimensions, exact aligned pitch, exact one-slice byte count and
256-byte base alignment. Unsupported or inconsistent output is failure.

The separate address-free API is declared in
`include/hackynavi/gfx11_color_target.h`. Its conceptual fields are:

```text
TopologyInput { rawGbAddrConfig, rawRevisionStrap }
TargetRequest { width, height }
TargetLayout { width, height, pitchPixels, pitchBytes,
               sliceBytes, totalBytes, baseAlignment, swizzleMode,
               cbColorInfo, cbColorView, cbColorAttrib,
               cbColorAttrib2, cbColorAttrib3, cbDccControl }
calculate(topology, request) -> owned layout or exact failure
validate(topology, request, layout) -> strict fixed-profile agreement
```

The API accepts numerical inputs; those values alone never prove observation or
ownership. Native capture authority remains outside this structure. The actual
adapter in `userspace/graphics/color_target.c` calculates this contract without
opening a GPU; `src/gfx11_color_target.cpp` checks every output field. A valid,
aligned output is cleared on failure; null, unaligned or wrapping output ranges
are rejected without writes. Input/output overlap rejects before input reads.
Invalid dimensions and unsupported topology fail before calculation.
No pointers, caller-supplied register stream or GPU address belong in this
layout contract.

The CB helpers receive layer indices/count-minus-one zero, mip zero/count one
and one sample. `radv_image_view.c:380..431` establishes the count-minus-one
semantics; blindly passing one for `num_layers` is incorrect. The helpers derive
RGBA component swap, UNORM/blend clamp and GFX11-specific format bits. DCC stays
disabled even though its harmless configuration fields can be nonzero. The
real mutable helper is initially called with address zero solely to produce
address-free configuration; this is a calculation, never a submitted address.

## Actual allocation and submission are separate work

The future native owner must take a connection-owned BO handle, offset and size,
prove same epoch/generation, writable render access and sufficient resident
allocation, reserve its lifetime, and resolve the real GPU VA itself. It must
revalidate the fixed profile and actual topology, alignment, VA range and full
surface coverage before constructing CB base/base-extension state. A numeric
layout from userspace cannot grant any of those permissions.

Publish no raw CB register list from a caller. Bind the image through one narrow
native image owner whose retained reference survives submission, cache flush,
actual GPU fence/RPTR retirement and CPU visibility. Prevent host writes and
conflicting GPU consumers during that interval. On ambiguous submission,
timeout, reset or removal, retain the existing native fault/reset behavior;
never recycle the allocation merely because host metadata says the draw ended.

Graphics admission still needs the real NGG/PS product, immutable draw state,
owned shader and geometry resources, producer exclusion, and actual queue
submission. The reviewed CB/DB barrier is one prerequisite; the existing queue's
final full-pipeline timestamp fence remains part of completion. This proposal
does not add a render encoder, display scanout, texture support or Metal render
capability.

## Focused validation

`scripts/check-gfx11-color-target.py` builds the isolated adapter against twelve
exact reviewed Mesa/addrlib archives. The report binds their binary identities,
49 included dependencies (14 manifest-verified Mesa files, one generated file,
32 toolchain headers and two authored inputs), preflight/actual dependency
equality and before/after hashes. The reused archives were not rebuilt from
their historical source; their provenance is explicitly limited to the prior
record and the exact reused bytes.

`build/gfx11-color-target-local-6/report.json` passed 493 actual calculator
products per strict and UBSan run. The products cover 14 dimensional cases,
including 4K and the maximum size, all 17 supported numerical pipe/packer pairs,
and both ends of the admitted revision range. A complete 512-case encoded
pipe/packer/interleave sweep rejects unsupported inputs before addrlib. Every
one-bit mutation of each 64-byte layout is rejected. The strict run passed
249,778 checks; the sanitizer run passed 248,349 (the difference is writing the
476 serialized records and closing that file only in the strict run). A
standalone portable run passed 245,248 checks. UBSan covers authored C/C++
adapter, validator and test code; reused Mesa archives are not instrumented.

The same report includes a freestanding x86_64 macOS 15 validator object with a
2 KiB frame limit. `build/gfx11-color-target-cmake-2/report.json` records a fresh
focused CMake build and CTest pass; the first CMake report is preserved with
failed provenance because concurrent, authorized display-clock build-list
changes occurred during it. No color-target test failed in that run.

The earlier `local-4` report retains the actual debug-library trap for numeric
64-pipe/64-packer input. Reviewing the real swizzle tables showed that addrlib
initializes all equations on creation and that this input exceeds their
75-entry D-swizzle table bounds. The final validator rejects that case before the library is
called. This was a host-library input-validation finding, not GPU behavior.

These are numerical API tests, not measured-device fixtures. A future actual
captured topology record must be explicitly distinguished from these inputs.
No draw or readback success is reported until the real native ownership and
hardware path exists and is tested on the target. The new portable source is
registered in the kext build without any native calculation or submission call
site; the actual Mesa calculator remains userspace-only.

```powershell
C:/Python314/python.exe scripts/check-gfx11-color-target.py --output build/gfx11-color-target-new
```
