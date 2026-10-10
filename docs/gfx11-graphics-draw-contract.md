# Narrow Navi 31 draw: source contract and numerical oracle

This work is the prerequisite for a real private offscreen draw. The reviewed
NGG/fragment admission module already validates the actual Apple-derived code.
The new oracle calls the real pinned Mesa host emitters and computes named RADV
register formulas. The separate canonical portable draw encoder now constructs
and exactly validates the fixed 712-DWORD draw stream. Neither component
observes hardware, allocates GPU resources, submits work, or proves rendering.
Native ownership, hardware execution and recovery integration remain required.

All Mesa references below are pinned to
`35b085c4c067f198ce0b6437d94678e47b9c645f` under `.research/mesa`.
The oracle is `tests/gfx11_graphics_draw_mesa_oracle.c`; the bounded local runner
is `scripts/check-gfx11-graphics-draw-oracle.py`. Its JSON scenarios are explicit
numerical inputs, **not captured Navi 31 topology**. The runner verifies actual
included materialized Mesa dependencies against the prior source manifest,
records generated/toolchain dependencies separately, and checks them after
compilation. It binds the exact reused archives and prior build report without
claiming to reconstruct those archives from source.

## Intended fixed draw

One auto-indexed triangle, three vertices, one instance, base vertex zero; one
linear `RGBA8_UNORM` target; full-target viewport and scissor. No descriptors,
vertex buffers, index buffers, varying exports, tessellation, user geometry
shader, scratch, LDS, depth/stencil, MSAA, blend, logic op, query, streamout,
textures, VRS, indirect draws, or caller-supplied packet/register fields.

The input is the immutable owned graphics image admitted by
`gfx11_graphics_product`, not an offline JSON report. The target's address-free
layout must pass `hn_color_target_validate` using the current native owner's
privately captured topology. Every code/target/ring binding must resolve under
the same live owner, epoch, page-table root and VMID; handles/generations, access,
single mapping and pairwise non-overlapping virtual extents must be checked by
the native owner. Numeric structures alone grant no authority.

The private IB allocation is 8 KiB and the CPU command capacity is bounded to
2,048 DWORDs. The current canonical stream is 712 DWORDs for every admitted
input. The native capture must clear the private allocation before copying and
publishing those bytes, and submit only the exact admitted length. This is an
internal native interface, not a public wire ABI.

## Topology and allocation requirements

| Value | Actual source and consumer | Ownership requirement |
| --- | --- | --- |
| `GB_ADDR_CONFIG` | `ac_gpu_info.c:454–465`: `num_tile_pipes = 1 << G_0098F8_NUM_PIPES(gb_addr_config)`; low three bits. Used by `PA_SC_MODE_CNTL_1.WALK_FENCE_SIZE`. | Current same-owner two-register color capture already covers the raw value; derive the field, do not accept it from a client. |
| Maximum SE count | `ac_gpu_info.c:1214`, from kernel `num_shader_engines`; sizes the attribute-ring allocation. | Strict GC discovery-table extraction is still needed; the existing parser deliberately treats that auxiliary table as opaque. |
| Maximum RB count | `ac_gpu_info.c:1223`; `ac_cmdbuf.c:615–625` emits `PIXEL_PIPE_STAT_CONTROL` with `(1ULL << max_render_backends)-1`. It also selects the small-chip DCC cache policy. | Requires the trusted GC table and same-owner native state. This is a maximum, distinct from harvested active RB count. |
| Good CU count per SA | `ac_gpu_info.c:1244–1313`: sum the per-SE/SA CU-mask popcounts; derive nonempty SE count; for GFX11 use `floor(num_cu/(num_se*max_sa_per_se*2))*2`. | Requires serialized GRBM selection and harvested mask observations; current color capture does not provide this. Do not substitute the smallest bitmap popcount or advertised marketing CU count. |
| Tile steering | Mesa `ac_gpu_info.c:1222` consumes the kernel field. Pinned Linux `gfx_v11_0_constants_init` assigns `pa_sc_tile_steering_override = 0`; this is software policy, not a read of the graphics context register. | The native profile should retain the source-proven zero policy; no invented observation/default. |
| `address32_hi` | `ac_cmdbuf.c:490,500`, `ac_cmdbuf_cp.c:299`: ES/LS high address and the asserted attribute-ring VA window. | Chosen by the real VM allocator. Code and attribute ring must share the selected 4 GiB window; it is not a GPU-discovery fact. |
| Attribute ring | `ac_gpu_info.c:1737–1767`: on GFX11 with L3, `align(1400*1024,65536)` bytes per maximum SE, total at most 16 MiB. | Distinct retained RW allocation and mapping, virtual base aligned 64 KiB; GFX11 has zero separate POS and PRIM offchip rings. Six maximum SEs require 8,650,752 bytes. No ring omission merely because the shaders export no parameters. |
| Mapping page size | `ac_cmdbuf_cp.c:306–309`: `BIG_PAGE` follows actual `discardable_allows_big_page`. | Existing private VM uses 4 KiB pages, therefore this profile explicitly encodes `BIG_PAGE=0`; virtual base alignment does not establish 64 KiB PTE support. |
| `pc_lines`, PBB allocation | `ac_gpu_info.c:1337–1340`: GFX11 `pc_lines=1024`, `pbb_max_alloc_count=16`. | Pinned architecture policy facts, not caller-selected topology. |
| CU restriction mask | `ac_gpu_info.c:80`: `spi_cu_en=~0`, optionally changed by Mesa user policy. | The fixed native profile has no user CU override. This software restriction is separate from harvested CU observations. |

The native topology worker owns any later extension of the present color
capture. The draw worker must not reinterpret the current two-register result
as complete graphics topology. Native ring allocation, reuse/retirement and
recovery remain required even after command bytes compare with the oracle.

## Complete-state audit table

This is the source checklist used by the canonical encoder. Register writes are
named and derived;
the libdrm test's opaque cached PM4 blob is not an implementation source.

| Stage | Required operations/state | Pinned implementation |
| --- | --- | --- |
| Context start | `CONTEXT_CONTROL` update load/shadow enables for non-shadowed queue. **No `CLEAR_STATE` on GFX11.** | `radv_queue.c:688–711`; `ac_gpu_info.c:1129`. |
| Common graphics preamble | PS/HS CU masks and wave limits, request grouping, all PS/ESGS/LSHS user accumulators zero, ES/LS program high; explicit generic/screen scissor defaults, tile steering, VRS/coverage/barycentric/point/NaN/line/stereo/primitive defaults; CB/DB cache policies; disabled texture border-color base; downconvert control; tess defaults; bin allocation, NGG deallocation limit; vertex/index/instance defaults and pixel-pipe stat control. | Real `ac_init_graphics_preamble_state`, `ac_cmdbuf.c:392–627`. Oracle emits the real function's command stream, not a copied array. |
| RADV additions to preamble | All 16 viewport Z ranges 0..1, edge rule `0xAAAAAAAA`; NGG vertex reuse depth30; point size/minmax; small-primitive compatibility; interpolation control; vertex pixel-center/round-even/16.8 quantization; viewport scale/offset enable; line stipple scale1; Navi31 FDCC single-clear-code workaround. | `radv_queue.c:718–827`. These remain separate from the common emitter. |
| Attribute ring transition | Adjacent `BOTTOM_OF_PIPE_TS` PWS release and acquire at **CP_ME**, distance0, before any ring writes. Then `SPI_GS_THROTTLE_CNTL1/2`, attribute base and size. | `radv_queue.c:612–626`, actual `ac_emit_cp_release_mem_pws`, `ac_emit_cp_acquire_mem_pws`, `ac_emit_cp_gfx11_ge_rings` at `ac_cmdbuf_cp.c:295–310`. Producer exclusion must keep the pair adjacent. |
| Instruction visibility | Real `ac_emit_barrier` with ICACHE/SMEM/VMEM/L2 invalidation and PFP/ME ordering. The previously reviewed completion barrier deliberately does not cover this. CPU writes/DMA synchronization and HDP visibility precede submission under the real owner. | `ac_barrier.c`, `ac_barrier.h:23–70`; oracle emits this separate real slice. |
| NGG subgroup | Vertex stage, triangle input, no GS, target group128, max256, wave32, no LDS/stride/scratch: actual `ac_ngg_compute_subgroup_info` returns ES128/GS128/output128 and zero LDS. | `radv_shader_info.c:1318+`; `ac_shader_util.c:1352+`. Oracle calls the actual function. |
| NGG program | Bind ES program address, admitted GS RSRC1/2; derive GS RSRC3/4, no-PC-export VS config, IDX1COMP/POS4COMP, no extra VS outputs/primitive ID; output/subgroup count128, amp1, instance count1; GE group size252; baseVertex user SGPR s8=0 for the admitted adapter ABI. | `radv_shader.c:1856–1981`, `radv_cmd_buffer.c:3400+`; exact adapter argument ABI in `userspace/compiler/ngg.c` and graphics admission. Do not copy RADV's additional user arguments absent from our actual shader. |
| Late allocation | Actual `ac_compute_late_alloc` returns0 and CU maskFFFF for <=2 good CUs/SA; otherwise GFX11 no-cull/no-scratch returns63 and clears CU1 (maskFFFD). PC oversubscription is lines/4 if late allocation is enabled. Preserve Mesa's masked unsigned field when lines=0. | `ac_shader_util.c:786–837`, `radv_shader.c:1884–1926`. A wrong unharvested default can cause a hang. |
| PS program | Actual address, admitted RSRC1/2, RSRC3 from common preamble, RSRC4 prefetch/CU mask; exact PS input enable/address/line-stipple dummy input, W32 control, zero Z exports, MRT0 four-component format and shader mask. No kill/memory/depth output; early-then-late Z policy. | `radv_shader.c:2010–2080`, `radv_cmd_buffer.c` fragment program emission; graphics admission report provides only its proven subset. |
| Viewport and scissor | Full-target Metal viewport: X scale/offset `width/2`, Y scale `-height/2`, Y offset `height/2`, Z scale1/offset0; Z bounds0..1; explicit screen/window/generic/viewport0 scissor and window-offset disable. GFX11 BR is exclusive. Logical dimensions are positive integers; no client float state is accepted. | `radv_cmd_buffer.c:4463–4613`, `radv_pipeline_graphics.c:641–659`; actual Apple shader output preserves clip Y. Encoder derives exact binary32 half-integers using integer arithmetic. |
| Raster/primitive | Triangle-list primitive type, triangle output, first provoking vertex, no cull/offset/stipple; clip enabled with 0..1 depth, no discard/VRS; guardband/discard factors; `PA_SC_MODE_CNTL_1` tile walk/fence settings with OOO disabled. | `radv_cmd_buffer.c:3790+`, `4854–4890`, `13395–13500`. |
| Sampling/binning | One sample, both AA masks all ones; center sample location and both centroid priorities0; EQAA static/high-quality policy; no conservative rasterization except null-squad AA behavior; AA config0, alpha-to-mask disabled; alternate RBs and viewport scissor; binning disabled with 128x128 extents. | `radv_cmd_buffer.c:2975–3015`, `13594+`; `radv_device.c:970–972,1009–1069`. |
| Color | Exactly validated linear RGBA8 layout; GFX11 CB0 VIEW..FDCC four-register block, then BASE/EXT, DCC_BASE/EXT and ATTRIB2/3 separately; CB1..7 invalid. No CMASK/FMASK register writes. Target/shader maskF, copy ROP/no blending, all blend controls disabled, explicit RB+ downconvert/epsilon/control and MRT blend optimization. | `radv_gfx11_emit_fb_color_state`, `radv_cmd_buffer.c:4931–4969`, `13502–13591`; actual `ac_surface`/addrlib product and portable `hn_color_target_validate`. |
| No depth/stencil/query | Invalid Z/stencil format, disabled depth/stencil control; render/count/override controls and occlusion-query state explicit. No inherited address may become accessible. | `radv_cmd_buffer.c:5290–5410`, `13230–13393`. |
| Draw | `NUM_INSTANCES=1`, auto-index source, `DRAW_INDEX_AUTO` vertex count3. No predicate, opaque counter, indirect argument, index address, streamout or caller packet stream. | `radv_cmd_buffer.c:11712+`, `14416+`; primitive type is triangle list, not libdrm's rect-list test. |
| Completion | Existing reviewed 16-DWORD CB/DB flush + L2/metadata WB/INV + PWS acquire at PFP. Only then may the real queue's GPU fence and consumed RPTR establish completion. | `gfx11_graphics_barrier` and its actual-Mesa oracle. Numeric equality is not a completed draw. |

## Shader prefetch and immutable pages

GFX11 has `instr_prefetch_distance=3` (`ac_gpu_info.c:1664–1665`). The real
`ac_align_shader_binary_for_prefetch` adds 192 bytes and rounds to128;
`ac_get_instr_prefetch_size` divides by128 and caps at63. For the actual
Apple NGG124-byte executable the extent is384 bytes, and for the 36-byte PS it
is256 bytes. The 256-byte stage-placement alignment does not mean that only
the executable bytes may remain mapped: all of the immutable 4 KiB owned code
image remains RX and resident. A prefetch can overlap the other immutable
stage, but cannot reach unmapped memory. The full page also covers the maximum
admitted two-stage layout and padding. Mesa's comment explicitly warns that
prefetching an unmapped page faults (`ac_binary.c:121–153`).

The guardband decision is intentionally conservative and explicit: hardware
screen offset0, all four clip/discard bounds1.0. `ac_guardband.h` defines these
as the clipping and culling boundaries in clip space; using the viewport edge
is correct and can require more clipping than Mesa's optional wider guardband.
With the admitted 16.8 quantization, coordinates 0..16384 fit the signed
65536-wide representable range. No floating point instructions are needed in
the kernel: width/2 and height/2 are converted to exact binary32 bits using
integer arithmetic, checked over all 16,384 supported widths and heights.

### Metal coordinate correction

Metal maps NDC Y=+1 to the top of a viewport whose pixel origin is upper left.
See Apple's [coordinate systems discussion](https://developer.apple.com/library/archive/documentation/Miscellaneous/Conceptual/MetalProgrammingGuide/Render-Ctx/Render-Ctx.html).
The actual Apple-generated `hn_triangle_vertex` translates without a Y flip:
its three position constants retain Y=-1,-1,+1, and SPIR-V stores the selected
vector directly to Position. The translator's separate passthrough shader test
does not describe this genuine AIR translation path.

The canonical Metal stream therefore negates hardware Y scale exactly once.
X, positive Y offset, Z=0..1, scissor and the actual shader bytes stay unchanged.
The oracle extracts the verbatim pinned `radv_get_viewport_xform` implementation
and calls it with Vulkan y=height/height=-height, the equivalent transform of a
positive-size Metal viewport. Its six real computed register values must match
all 15 actual-product streams. Each stream is also compared with preserved
`gfx11-graphics-draw-local-4`: exactly one DWORD changes, only its Y sign bit.
That historical positive-Y stream was unsuitable for Metal and is superseded.
This correction is encoding evidence, not a GPU rendering result.

## Portable validation evidence

The local runner compiles the new module with strict warnings and UBSan, checks
every bit of the resulting canonical stream, every truncation/capacity and
address-free target-layout bit, owner/generation/access/VA overlap negatives,
CPU publication aliases, topology/CU boundaries, shader mutations, and exact
float-bit conversion. A successful workspace never bypasses re-admission.

It independently decodes 15 command streams constructed from actual frozen
Apple and authored NGG/fragment products. Each must contain the exact register
sequence from the real Mesa common preamble, the named RADV specializations,
and byte-identical real idle/ring, instruction visibility and graphics
completion slices in the correct order around the actual draw packet.
The six viewport registers also match the real extracted RADV transform.
The dynamic CB sequence and values also match real addrlib surface calculation
and `ac_init_cb_surface`/`ac_set_mutable_cb_surface_fields`, with the exact
named GFX11 register sequence. Every emitted register address is checked against
the pinned `gfx11.json` database. Two explicit database omissions are admitted
only with their pinned GFX11 emitter evidence: `SPI_BARYC_SSAA_CNTL` at
`ac_cmdbuf.c:502` and `GE_VRS_RATE` at `radv_cmd_buffer.c:4820`.
The separate host oracle exercises 12 numerical scenarios and 204 checks.
The strict freestanding and actual MacKernelSDK cross-object builds use a
2 KiB frame limit. These prove encoding and rejection properties; the
topology test values are not captured hardware and no GPU completion is faked.

The preserved `build/gfx11-graphics-draw-local-2` evidence is superseded for
draw correctness: independent review found that its CB setup used the GFX10
11-word BASE block and CMASK/FMASK high registers. Eight resulting addresses
do not exist in the GFX11 database. The corrected runner reads that original
stream and proves that the full-address check rejects exactly those eight
writes; it never overwrites the earlier evidence.

## Remaining native integration

The actual existing queue is a graphics-ring IB transport, but its admitted
products currently dispatch compute. A real draw requires a new typed capture
that copies and admits the code image, resolves/rechecks target ownership,
allocates and retains the attribute ring, creates the immutable canonical IB,
seals mutation, and records all controls for actual completion consumption or
engine reset. Failure after hardware exposure must retain ambiguous owners.
No startup call site, GPU test, framebuffer scanout, or public Metal render
encoder is enabled by this source audit/oracle.
