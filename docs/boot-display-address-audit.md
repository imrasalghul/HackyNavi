# Firmware display address audit, 2026-10-11

The corrected core `9eda6a40f88c` loaded on the target. Its complete, stable
display snapshot remains insufficient to admit inherited scanout reservations.
The decoder corrections in this change do **not** enable engine startup on that
capture and do not change hardware. They remove four overly narrow predicates
while preserving two unresolved target blockers.

Evidence is `live-9eda6a40f88c-20261011-153308-790/device.plist`, SHA-256
`e371164d350d1bbad52d0cf76fd235956a860933d7f356c62dd82413147162c5`.
`tests/fixtures/boot_display_9eda.hpp` contains only its raw display registers
and memory-layout inputs. The local validation script compares every fixture
value and offset against that exact plist before compiling the replay test.
An offline replay cannot establish current hardware state or ownership.

## Corrections supported by Linux's DCN3.2 path

All source references are pinned to Linux
`3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0`; local source hashes are checked in the
validation receipt against `.research/linux/boot_display_sources.json` and
`navi10_enum.h.source.json`.

- `DCN_VM_AGP_BOT=TOP=0` represents the inclusive address window
  `0x00000000..0x00ffffff`, not an encoding for a disabled aperture. The decoder
  permits this window only because it is completely disjoint from the exact
  validated framebuffer MC window. Any overlap remains rejected without relying
  on the unobserved AGP base. The framebuffer base, top and offset must still
  match the independent memory layout exactly.
  [DCN3 hubbub aperture programming](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/hubbub/dcn30/dcn30_hubbub.c)
- `TILING_CONFIG=0x80` has `SW_MODE=0` and `DIM_TYPE=1`: linear 2D, not a tiled
  surface. DCN3.2 selects `hubp3_program_surface_config`; its tiling function
  programs swizzle and metadata fields but leaves the dimension field unchanged.
  Linear dimensions 1D and 2D now share the bounded row calculation; 3D,
  reserved dimensions and every nonzero data swizzle remain rejected.
  [DCN3.2 function table](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/hubp/dcn32/dcn32_hubp.c),
  [DCN3 tiling programming](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/hubp/dcn30/dcn30_hubp.c),
  [dimension enum](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/include/navi10_enum.h)
- `FLIP_CONTROL=0x04100000` contains a pending-delay configuration of 65,
  not `SURFACE_FLIP_PENDING`. The latter is bit8. Delay bits29:20 are permitted
  while pending, update/master locks, stereo and GSL/triple-buffer policies stay
  rejected. Both passes must still observe the exact timing configuration.
  [DCN3.2 masks](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_2_0_sh_mask.h)
- DCN3.2 uses `hubp2_is_flip_pending`, which checks `SURFACE_FLIP_PENDING`
  and compares `EARLIEST_INUSE` with the requested surface address. It does not
  use the separate ordinary `INUSE` latch for this decision. The decoder now
  always requires primary and earliest addresses to agree; ordinary INUSE must
  additionally agree when `SURFACE_INUSE_RAED_NO_LATCH=1`. All raw values,
  including latched INUSE, remain subject to two-pass stability. Target pipe0
  primary/earliest are `0x8000000000`; its ordinary INUSE is zero and
  `FLIP_CONTROL2=0x40` leaves NO_LATCH clear.
  [DCN2 flip-pending and state reads used by DCN3.2](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/display/dc/hubp/dcn20/dcn20_hubp.c)

These corrections do not prove that firmware is excluded from later updates.
The existing external exclusion requirement applies throughout capture and use
of any computed intervals. A footprint is an exclusion interval, not ownership
of the framebuffer object or an inventory of all firmware/display allocations.

## Target blockers retained

The target has `VMID=0`, `TLB_CONTROL=0`. The source-backed Linux DCN3.2 setup
programs `ENABLE_L1_TLB=1`, `SYSTEM_ACCESS_MODE=3`, raw `0x19`. Older AMD headers
name access mode zero `PA_ONLY`, but that does not establish DCN3.2's address
domain or whether its bypass path applies the MC framebuffer offset. No such
inference is admitted. The decoder still requires exact `0x19` and returns
`UnsupportedAddressMode` for this target.

Pipes1,2,3 are powered, with controls `0x000f001a`, `0x000f002a`,
`0x000f003a`. Bits7:4 select VTG1,2,3; they are **not TTU_DISABLE**.
TTU_DISABLE is bit12 (`0x1000`) and is clear, as is BLANK_EN bit0. IN_BLANK bit3
and NO_OUTSTANDING bit1 are status observations; neither prevents a later fetch.
Their zero viewport and pitch fields are not a proof that a powered, unblanked
requester is disabled. The implementation therefore does not silently skip them.

The tests deliberately separate these facts:

1. Exact archived registers reject at pipe0, exposing a complete raw snapshot
   but no plane or reservation output.
2. An explicitly artificial variant changing only pipe0 TLB to `0x19` reaches
   pipe1 and still rejects its address mode.
3. Artificial variants isolate each unblanked, zero-viewport pipe with TLB
   `0x19`; each rejects without publishing previously decoded planes.
4. Only an artificial variant supplying `0x19` plus explicit blank-and-drained
   controls for pipes1..3 permits a 1920x1080 RGB32 surface with 1920-pixel pitch,
   MC address `0x8000000000`, and an 8,294,400-byte interval at VRAM offset zero.
   These are test inputs, not hardware writes, observed target changes, or a
   claim that the user's 4K165 monitor is running a 4K165 firmware framebuffer.

## Local validation

Run `scripts/check-boot-display-address-local.py --output <fresh-directory>
--capture <exact-device.plist> --discovery .research/rom/discovery-00195d00.bin`.
It binds actual capture values, checks pinned register masks and source hashes,
runs the decoder and display-stop regressions under strict C++11 warnings and
UBSan, then compiles both portable units against real macOS kernel headers with
a 2KiB stack-frame budget. Receipts state that no GPU, target or remote execution
occurred. This does not replace a native bundle build or target testing.
