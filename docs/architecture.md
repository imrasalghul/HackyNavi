# Architecture and implementation boundaries

This milestone implements Navi 31 discovery and memory initialization components. The running service does not yet call the GART/HDP programming helpers or execute work on the GPU.

## Current components

```text
Supplied ROM file ──► portable bounded parser ──► JSON inspection report
                            ▲
Existing macOS ROM property│
                            │
IOResources observer ────────┘
        │
        └── PCI configuration reads ──► historical IORegistry snapshots

Existing Linux sysfs attributes ──► bounded collector ──► reference JSON

Opt-in PCI service ──► assigned BAR5 ──► serialized VRAM selector reads
                                               │
                                               ▼
                              bounded acquisition + discovery parser
                                               │
                                               ▼
                                      IORegistry IP inventory
```

### ROM parser

`include/hackynavi/rom.hpp` and `src/rom.cpp` have no allocation, hardware access, or input writes. Byte reads use explicit little-endian decoding rather than packed structure casts. Bounds checks use subtraction to avoid offset-plus-length overflow. Nested tables are bounded by their enclosing PCI image, even when more bytes remain in the full flash dump.

The caller's image descriptor is reparsed before accessing ATOM data. Only supported revisions are interpreted; unsupported revisions preserve the documented validated descriptor fields. Memory module and display path counts are capped. Connector record traversal must advance, and firmware part numbers are bounded, sanitized, and terminated.

The parser recognizes the shared ATOM 2.2/2.3 header prefix and master data 2.1, VRAM 3.0, display 1.5, FirmwareInfo 3.4/3.5 and VRAM usage 2.1/2.2. The reservation decoders distinguish confirmed absence, unsupported revisions, malformed structures and decoded payloads. Firmware tail/protected-region sizes and usage address flags are reported directly; usage 2.1 has no explicit driver start address and none is invented. The VRAM 3.0 bus width follows Linux's `channel_count * 16` interpretation. Memory entries are alternative firmware definitions; they do not identify populated chips. Display paths do not establish connected monitors or active scanout. An image checksum is not an authenticity check.

`tools/rom_inspect.cpp` limits input to 16 MiB and emits JSON. Image discovery skips structurally invalid candidates and can find separate image chains inside a full dump. It does not establish complete flash integrity or a physical dual-BIOS arrangement.

### macOS observer

The `HackyNavi` class is an `IOService` attached to `IOResources` in its own matching category. It subscribes to first-match notifications for `IOPCIDevice` and filters registry identities before reading configuration space. Configuration identity is checked again against `1002:744c`.

The observer records revision, class, subsystem identity, configured BAR values, and metadata from existing `ATY,bin_image` or `rom-image` OSData properties. It makes a bounded private copy of a ROM property before parsing. It limits ROM data to 4 MiB and snapshots to 16 devices. BAR lengths, usable VRAM, active IP versions, and running firmware are not inferred from those values.

Notifications can invoke the callback synchronously during registration, so callback state is initialized first. Matching dictionaries retain/release according to `addMatchingNotification` ownership rules. Shutdown marks the service as stopping, then removes/drains the notifier outside the callback lock before releasing state. Published snapshot dictionaries are immutable. Snapshots are historical first-match observations, with no hot-unplug tracking.

This observer has no GPU provider attachment, user client, BAR mapping, MMIO access, PCI configuration write, ROM enable, firmware loading, ATOM command execution, interrupt handling, or power-management implementation. Neither service exposes a framebuffer, IOAccelerator, or Metal device.

### Opt-in PCI discovery service

`HackyNaviDevice` matches `1002:744c` only with `-hackynavi-device`. It opens the provider, validates the current configuration, maps assigned BAR5 uncached and serializes acquisition under a lock. It requires memory decoding already enabled and does not enable bus mastering or size-probe BARs. A start failure releases all mappings, buffers, locks and provider ownership.

The service joins its provider's IOKit power tree with off/on states. BAR register access is deferred from `start()` to the on-state callback after the parent powers up; configuration/D0 and the assigned BAR mapping are rechecked on each power generation. The same mutex covers the complete acquisition and gates power-down. Sleep and shutdown remove the old discovery/memory/display snapshots. Teardown sets the stop/access gates, calls synchronous `PMstop()` outside the mutex, then drops buffers, mapping and PCI ownership. Matching can complete before acquisition; `SnapshotStatus` distinguishes waiting, validated, failed and powered-off states. This follows the [IOKit power-management contract](https://developer.apple.com/library/archive/documentation/DeviceDrivers/Conceptual/IOKitFundamentals/PowerMgmt/PowerMgmt.html), with real-header compilation and independent source review only. The service currently enables no GPU work, so an off callback has no submitted work to drain. Integrating the GART controller must add verified producer shutdown and allocation retirement before acknowledging power loss.

The portable `device_discovery.hpp` / `.cpp` module accepts ordered MMIO callbacks with byte offsets. It polls the firmware-ready bit, reads VRAM size, then reads the discovery header at VRAM end minus 64 KiB. The default containing region is 10 KiB. If that signature is absent, it checks Linux's driver-scratch descriptor for another bounded VRAM range. System-memory discovery is unsupported. Poll counts/delays, alignment, address arithmetic, region bounds, declared binary length and caller capacity are checked. A transport error leaves an explicitly incomplete capture that must not be parsed. A complete capture is passed to both the inventory parser and the required-IP layout builder before any derived register access. After successful memory capture, the service retains the bounded private binary and its actual source/containing-region metadata for later initialization and reservation accounting. It also resolves and reports HDP layout status; an unsupported HDP layout does not invalidate the separate discovery report, but cannot be used to construct the GART controller.

### Register resolution and memory domains

`ip_layout.hpp` / `.cpp` require the known Navi 31 GC 11.0.0, MMHUB 3.0.0, MP0/MP1 13.0.0 and SDMA0/1 6.0.0 records. Duplicate/missing required records, unsupported topology/version/encoding, and insufficient base arrays are rejected. Normal register resolution adds the generated DWORD offset to its discovered base index, then validates a complete 32-bit access inside BAR5. SDMA 6 uses GC base[0] with engine stride `0x600`, except internal offsets `0x5880..0x589a`, which use GC base[1] and stride `0x20`, matching Linux. The required-IP profile does not decode auxiliary harvesting/topology tables or establish all GPU capabilities.

After that check, the PCI service reads MMHUB 3.0.0 `FB_LOCATION_BASE`, `FB_LOCATION_TOP` and `FB_OFFSET` at generated offsets `0x08ec`, `0x08ed`, `0x08d7`, all base index 0. A dedicated stage flag permits only these resolved reads; selector writes remain disabled. A second pass detects observed changes but is not an atomic snapshot. BAR0's assigned descriptor must match its configured address; BAR0 is not mapped or resized.

`memory_layout.hpp` / `.cpp` decode the 24-bit fields in 16 MiB units and validate configured VRAM against the MC window and the 48-bit physical range. GPU MC placement (`vramStart`), GPU physical page base (`vramPageBaseOffset`) and CPU BAR0 physical address remain separate. CPU-visible size is the smaller of BAR0 and actual VRAM. Range helpers produce either MC or physical page addresses from VRAM-relative offsets. These ranges are not allocation pools: display, PSP, firmware and discovery reservations still need establishing.

### GPUVM encoding and DMA storage

`gpu_vm.hpp` / `.cpp` build ordinary 4 KiB PTEs and directory pointers as explicit little-endian CPU bytes. Contiguous and scatter operations validate all inputs before writing, including capacity, alignment, complete address spans and input/output overlap. GMC11's 48-bit VRAM physical and 44-bit system DMA limits are distinct. Directory encoding implements the Linux translate-further rules (PDB1 BFS=9; PDB0 TF); callers must select the actual configured policy. The API omits large pages, PRT and implicit cache-policy changes. Effective memory type and snooping remain caller responsibilities.

`HackyNaviDMABuffer` owns a prepared `IOBufferMemoryDescriptor` and `IODMACommand`. Device addresses come from generated DMA segments, never from a cast CPU pointer or a manually assumed physical identity. The helper uses a resolved device mapper when declared; identity DMA is admitted only when no device/ancestor mapping declaration exists and the OS system-mapper state explicitly reports no mapper. Unresolved/ambiguous states and bounce buffers are rejected. It checks segment coverage, overlap, alignment and width, and pairs preparation with completion/unmapping on failures and teardown. A caller must stop all GPU references before teardown and keep the device open and DMA domain stable.

The DMA helper currently compiles but is not invoked by the PCI service. Its synchronization methods order coherent, snooped x86 DMA and use IOKit synchronization; they do not wait for GPU completion or flush GPU caches/TLBs. Neither the helper nor the page-table encoder installs a GART, assigns a VMID, enables DMA, submits work, or proves that a hardware mapping works.

`HackyNaviGARTStorage` owns separate prepared DMA allocations for a fault page, data, writeback and display runtime storage, plus a CPU-only flat page-table image. The default 512 MiB aperture needs a 1 MiB image; supported images represent 2 MiB through 1 GiB apertures in 2 MiB increments. Unbound entries preserve the dummy address with every flag clear, matching Linux's Vega10+ unbind policy: setting SYSTEM on an invalid entry would be wrong. Binding encodes only owned DMA segments as valid, snooped, readable/writable UC system pages, with executable permission explicit. Each binding has separate invalid guards. Temporary encoding precedes the single image commit; rejected binds leave the image and metadata unchanged. Unbind changes the CPU image only. GPU references must be removed before reset releases the allocations.

### VRAM ownership and allocation

`vram_allocator.hpp` / `.cpp` provide a bounded allocator over a caller-established owned arena, with 32 reservations and 64 live allocations. The caller rounds reservations outward to pages and intersects them with that arena before configuration; partially external or overlapping declarations are rejected. Alignment, range overlap, address domains and total sizes are checked before mutation. Owner-bound handles carry a monotonic allocation serial; stale, double-release and wrong-owner handles fail. The allocator provides GPU MC and physical address translations, but performs no mapping, zeroing or hardware access.

The arena cannot be inferred from BAR0 visibility or the MMHUB framebuffer window. An independent read of both supplied ROM images found FirmwareInfo 3.4 with `fw_reserved_size_in_kb=4096`, while VRAM-usage 2.1 contains zero start/firmware/driver fields. Those flash values do not describe every runtime reservation, and the GOP can update runtime tables. Linux separately accounts for pre-OS scanout/VGA, firmware tail, conditional memory training and driver allocations. Navi 31's PSP 13.0.0 selects separate TMR allocation rather than Cobalt64's PSP 14 boot-time reservation path. The current service therefore still publishes `ReservationsNotEstablished`; no allocator is configured from an assumed free region.

`vram_reservations.hpp` / `.cpp` now build the bounded reservation plan. Inputs retain ROM/runtime provenance and parser/capture status; known absence differs from unknown evidence. Firmware and protected tails, explicit usage intervals, the entire discovery source region, pre-OS surfaces and other supplied live reservations are rounded outward to 4 KiB and merged with provenance. Training-capable firmware adds the exact 4 KiB host and firmware blocks and requires inactive training; long training's temporary bottom-32-MiB damage is not confused with a permanent allocation. Usage 2.1 driver scratch remains CPU metadata. Missing evidence returns a conservative partial plan; malformed/overflowing input returns no plan. Completion means the declared evidence categories are present, not that firmware stopped or that the complement is owned VRAM.

`boot_display.hpp` / `.cpp` derive DCN 3.2 register addresses from the DMU discovery record and capture all four HUBPs twice through read-only callbacks. Supported linear packed RGB surfaces require coherent primary/current/earliest addresses, a verified MC framebuffer/aperture route, VMID0, no DCC/TMZ/alpha/stereo/pending flip, and bounded pitch/viewport data. Pitch is decoded as stored value plus one. The exclusion includes preceding viewport rows and pitch padding, rounded outward to pages. Active cursor, dynamic metadata, legacy VGA and other unsupported fetches fail without publishing intervals. This bounds the current scanout footprint, not the whole framebuffer object, DMCUB/MALL/writeback buffers or all display memory. Snapshot equality does not exclude another display programmer. These reservations may only be consumed while external power/modeset/ownership prerequisites hold. The PCI service now calls this capture diagnostically after memory capture and publishes `BootDisplay` with explicit status, per-plane offsets and `ReservationUseAllowed=false`. Unsupported display state does not invalidate the separate discovery/memory snapshot or authorize allocation.

### Bootstrap engine shutdown

Display capture also checks the separate DCN DOMAIN0..3 PGFSM status registers. PCI D0 alone does not establish HUBP power. A non-on or transitioning domain returns an incomplete capture without reading that HUBP's register window. Power status is checked around each pass and immediately before each HUBP read; no power-gate control is written. These observations cannot exclude an uncoordinated external power actor, and a powered-off pipe is not assumed to have no older requests outstanding.

`bootstrap_quiesce.hpp` / `.cpp` stop CP and both SDMA6 engines using the selected legacy/RS64 architecture. They resolve every address before I/O, reject nonzero PSP sOS sign-of-life, enabled VM contexts, active MES and unsupported SDMA queues, then halt CP, disable queue0 RB/IB, halt both SDMA F32s and verify controls/idle with bounded polls. Guards are repeated after the stop. Failure reports whether writes may have occurred; no rollback or automatic resume is invented. This is a CP/SDMA component, not a whole-GPU ownership token: VCN/JPEG, LSDMA, display/DMUB, PSP/SMU and reset/POST remain separate parts of takeover. The helper is not called by the service yet.

### HDP and GART register protocols

`hdp.hpp` / `.cpp` resolve NBIO 4.3.0 base index 2 from a complete validated discovery binary. They enforce the bare-metal x86 4 KiB remap policy: the reserved BAR hole starts at byte offset `0x7f000`. Configuration programs both HDP remap controls and verifies readback. A flush verifies those controls still match, writes zero to the memory-flush hole, and posts that write by reading NBIO memory size. Failed MMIO or mismatched/unavailable hardware latches a fault; clearing bookkeeping is allowed only after the owner independently quiesces/resets hardware. Ordered uncached callbacks and prior CPU-upload ordering are required. This is the CPU HDP protocol, not a GPU completion fence or TLB invalidation.

`gart.hpp` / `.cpp` implement a separate plan for GC 11.0.0 GFXHUB or MMHUB 3.0.0. Preparation validates every resolved register and memory range, then captures bounded ordinary reads for read-modify-write operations before any write. Plans are noncopyable and single-use. Enable programs a contiguous owned VRAM leaf table, an owned VRAM default page and a 44-bit owned dummy DMA page. The aperture is 4 GiB aligned, 4 KiB through 4 GiB in size, below the 48-bit VA hole, and outside the framebuffer window. This path disables all 16 contexts before enabling VMID0 with flat 4 KiB tables; AGP, identity translation and translate-further are disabled.

The sequence configures hub apertures/caches and complete invalidation ranges, requires HDP flushing, then issues engine 17 VMID0 invalidation with bounded polling. MMHUB uses its read-to-acquire semaphore and private CID2 invalidation posting read; GFXHUB uses the direct register path. Standalone flush/disable plans explicitly set engine 17's full range rather than trusting inherited state. A failed read can leave semaphore ownership uncertain; a known-held semaphore gets a best-effort release. Results distinguish that uncertainty from failed writes. Hardware changes are not rolled back, and allocations must remain resident until recovery establishes that the GPU no longer references them.

These helpers require exclusive powered-device/register ownership, quiesced clients, initialized memory controller/common IP, uploaded resident allocations and serialized execution. GFX must be held awake with no KIQ/MES/GFXOFF path; SR-IOV and XGMI are unsupported. MMHUB normally becomes active during GMC initialization before full PSP/GC firmware loading. GFXHUB follows during GFX initialization after CP idle and RLC bootload completion. The service has not established these prerequisites, so it does not execute either protocol yet. Simulated MMIO verifies software ordering, bounds and failure reporting; actual table upload, hardware invalidation and DMA remain unverified.

### Kernel GART controller and VRAM upload

`vram_transfer.hpp` / `.cpp` adapt Cobalt64's MM_INDEX_HI / MM_INDEX / MM_DATA write path. Every transfer resolves a live allocator handle rather than trusting a supplied range. It validates the complete aligned subrange, CPU buffer and BAR extent before touching hardware. Upload writes the entire buffer, then independently selects and reads every DWORD to detect data/addressing mismatches, including aliases that immediate per-word readback could miss. The input image remains immutable. Transfers are bounded to 2 MiB and report partial data/selector writes and verified bytes separately. All-ones payloads are legitimate data. HDP visibility and GPU completion are separate operations.

`HackyNaviGARTController` is the kernel integration of these components. It retains an existing open provider/owner and assigned uncached kernel BAR5 mapping, derives IP/HDP layouts from the supplied discovery snapshot, configures a private allocator from externally established arena/reservations, and allocates real DMA and VRAM storage. Data, writeback and display receive guarded mappings. `upload()` writes and verifies the VRAM table and zeroed default page through actual volatile MMIO callbacks. It then supports separate MMHUB and GFXHUB activation, with HDP visibility before publishing a root address. PCI memory decode/D0 are checked; bus mastering must already be enabled before hub activation.

The controller takes a self-reference before any programming callback can change hardware. A partial enable/flush/retirement failure changes state to `FaultedRetained` and preserves DMA mappings, VRAM handles, the provider, open owner and BAR mapping. Successful retirement requires the caller to quiesce all GPU producers first, then disables GFXHUB before MMHUB and releases allocations only after verified disable. `releaseAfterDeviceReset()` performs resource cleanup after a separately verified reset; it does not itself reset hardware. Dropping the caller's object reference cannot bypass the hardware hold. The enclosing service must also defer PCI close and DMA-domain teardown until retirement succeeds.

This controller has real kernel-header compilation and source-review evidence. It is not yet called by `HackyNaviDevice`: runtime arena ownership, GPU power/reset coordination, producer quiescence and recovery remain integration work. Its MMIO callbacks are actual BAR accesses, with no simulated backend selected in production code. Host tests exercise the portable protocols; they are not presented as controller runtime or GPU execution results.

VRAM reads write `MM_INDEX_HI` and `MM_INDEX`, then read `MM_DATA`. Selector state is not restored because the references provide no verified readback/restoration protocol. The caller must serialize the entire acquisition, own a powered device, and prevent concurrent reset/removal. Opening an IOPCIDevice alone cannot establish every platform-level condition. These prerequisites and real MMIO behavior still require macOS hardware validation. This path is separate from the passive observer and does not run by default.

### IP discovery and firmware inputs

`discovery.hpp` / `discovery.cpp` decode binary header versions 0/1/2 and IP table versions 1–4. The directory and table spans, additive checksums, die boundaries, variable-length IP records, and raw 32/64-bit base-address fields are bounded before invoking a visitor. A late malformed record produces no partial callback stream. Auxiliary table contents remain opaque; passing their checksum does not validate their internal schemas. The caller must keep input immutable throughout parsing and callbacks.

The returned base addresses preserve the firmware encoding. They are not mapped CPU addresses. Linux applies hardware-specific interpretation when selecting registers; this parser deliberately does not make that decision. A real Vega 10 sample validates version 2 parsing but is unrelated to the installed Navi 31. The user's ROM supplies version 3 format evidence. Versions 1/4 and 64-bit addresses currently have synthetic coverage only.

### Target ROM discovery evidence

The supplied 2 MiB ROM has SHA-256 `0998f9e7d59f819a7fc6f424f1d97eb65012e886972d3af28d0e79dab27f0f6a`. Two byte-identical discovery payloads occur at file offsets `0xB5D00` and `0x195D00`. Each is 1,756 bytes with SHA-256 `e52312475450105abdf58683db2016cb5b407b617cb6ba78b4f10830cd37d34b`. They pass the parser's checksums and structural checks: binary version 1.4, IP table version 3, one discovery die entry and 61 IP records.

Named records include GC 11.0.0, SDMA0/1 6.0.0, MP0/MP1 13.0.0, MMHUB 3.0.0, DMU/DCN 3.2.0, NBIF 4.3.0, HDP/OSSSYS 6.0.0, six UMC 8.10.0 instances and VCN 4.0.0/4.0.1. Hardware IDs use the pinned Linux `soc15_hw_ip.h` definitions. These are embedded firmware descriptions, not live enabled topology, firmware readiness, usable register mappings or an exact physical die count. The duplicated payload is not evidence of two physical BIOS chips.

`hackynavi-discovery --scan FILE` reproduces candidate discovery and validation directly from a ROM. It advances byte by byte regardless of untrusted declared lengths, including nested signatures, and reports invalid candidates too. Its 128-candidate and 4 MiB cumulative parser-window limits bound work; reaching either yields an explicit incomplete-scan status and exit 4. Each parser window is capped at the 16-bit binary format's maximum length. The original file is only read.

`firmware.hpp` / `firmware.cpp` decode the common AMD firmware header and exact GFX 2.0, SDMA 2.0 and PSP 2.0 layouts. GFX instruction/data and SDMA context/control ranges are file-relative; PSP image offsets are payload-relative. Real PSP files report a 36-byte fixed header followed by flexible descriptors, so those records are bounded against the pre-payload region rather than the fixed header size. SDMA jump-table offsets/sizes are bytes within their corresponding thread image.

Firmware family is a caller-supplied fact. Common header version 2.0 is shared by unrelated families and has no signature; it cannot identify a GPU. Stored CRC32 is metadata only, following the pinned Linux validator's lack of a CRC recomputation. Structural checks do not replace PSP authentication, IP-version matching, firmware selection, or the initialization sequence. The graphics loader additionally validates IMU1.0, RLC2.3 and SMU2.1 layouts for this Navi31 firmware profile.

`psp.cpp` implements the MP0 13.0.0 cold bootloader sequence. Each component occupies an owned 1MiB-aligned VRAM allocation and is uploaded, independently verified and HDP-published before mailbox submission. Non-SOS commands require the source-exact ready response; SOS requires a delayed, changing nonzero heartbeat. An existing SOS returns a distinct refusal rather than claiming this driver loaded it. Pinned Linux supplies KDB bytes for its SPL command; this behavior is preserved explicitly.

`psp_ring.cpp` implements the 4KiB KM ring, 64-byte command frames, single-flight fence/response processing, TOC-derived TMR sizing, TMR setup/destruction, firmware loading and RLC autoload. Ring/control/input/TMR storage must be distinct live handles. Nonzero firmware response status fails the operation. Ambiguous submissions retain their allocations; stopping the command ring does not itself retire TMR or loaded engines.

`gfx_firmware.cpp` validates the complete revision-C8 non-MES firmware set before uploading anything. It reuses staging only after each PSP command's actual fence and response. The official set produces 23 loads: SMU, SDMA context/control, RS64 instruction/stacks, IMU and RLC components. RLC autoload is followed by CP_STAT/BOOTLOAD_COMPLETE polling. Firmware acceptance is not CP queue initialization. `HackyNaviFirmwareController` connects these protocols to embedded firmware and GART's shared allocator/mutex; it also loads the SOS register-list component and blocks GART retirement while PSP resources remain owned.

`scripts/fetch-firmware.py` and `embed-firmware.py` verify pinned sizes and SHA256s, preserve the AMD license, and produce immutable linked resources. They neither execute firmware nor provide a substitute for PSP authentication. Firmware bytes are available during early kernel startup without a userspace resource daemon.

### Graphics initialization and submission

`smu.cpp` executes the normal MP1 mailbox protocol, queries actual version
responses, disables GFXOFF and verifies GC reset/bootload state. The kernel
firmware controller completes this before admitting GFXHUB or queue setup.
`gfx_init.cpp` uses the PSP-loaded RS64 path: per-pipe firmware start PCs and
reset pulses, the complete AMD GC11 clear-state stream, VMID0 constants and
owned RLC CSIB/SRM configuration. It does not substitute direct-loader cache
programming for PSP initialization.

`gfx_queue.cpp` programs legacy graphics pipe0, submits the full clear-state
stream and waits for a GPU-written RELEASE_MEM fence and consumed RPTR.
Subsequent trusted VMID0 IBs and CP memory writes use the same completion path.
It pads submissions to 256 DWORDs, excludes control-buffer aliasing and faults
on ambiguous publication/completion. It supplies no untrusted PM4 validator,
shader compiler, renderer or userspace API.

`HackyNaviGraphicsQueue` connects these modules to actual owned storage: a
16KiB GART ring, separate 16KiB IB, disjoint writeback fields and a 4KiB VRAM
CSB from the shared allocator. Every operation locks queue, firmware and GART
in that order. The exclusive Data/Writeback lease prevents concurrent SDMA
reuse and firmware/GART retirement. A GPU write test checks three independent
values plus the other words of a canary page. Stopping CP alone does not release
the RLC CSB. The PCI service has not yet invoked this owner on hardware.

### GFX11 shader execution

`gfx_compute.hpp` / `.cpp` encode trusted kernel clear/copy dispatches. The
two AMD GFX11 programs are fixed machine code from pinned libdrm 2.4.120.
`HackyNaviGraphicsQueue` allocates and verifies a dedicated 4KiB VRAM image,
publishes it through HDP, and retains it with the queue and RLC clear-state
allocations. Instruction prefetch is limited to three 128-byte units per
entry, within the owned allocation. Six-SE thread masks, VMID0 buffer
descriptors, wave64 dimensions and compute registers are explicitly encoded.
The graphics ring provides cache writeback, GPU-written fence and RPTR
completion; there is no CPU copy fallback.

The engine's shader verification initializes guarded DMA pages, submits a
4KiB clear and then a 4KiB copy, and compares all 5,120 words after each
completed dispatch. Startup cannot report Ready without the two dispatches
and 10,240 comparisons completing. This code is compiled and protocol-tested;
the test models do not establish that a physical shader executed.

### ATOM ASIC initialization

`atom_vm.hpp` / `.cpp` implement the ATOM instruction interpreter with a heap
workspace, bounded instructions/delays/nesting, control-flow validation before
I/O, little-endian parameter/scratch storage and explicit hardware errors.
`HackyNaviAtomController` binds to the same native MODE1 reset owner, validates
the captured board ROM, and executes its actual ASIC_Init table with bus
mastering disabled. It observes the table's real hardware completion read
in addition to SCRATCH7: the supplied bytecode sets that scratch marker even
after its polling counter expires. The native adapter does not implement
Linux's empty MC/PLL callbacks as successful operations; the reviewed board
table uses direct MMIO only. Engine recovery and service lifetime integration
remain separate from table execution.

### SDMA command generation

`sdma.hpp` / `sdma.cpp` provide allocation-free SDMA 6 copy, constant-fill, and fence/trap packet construction in caller-owned CPU memory. The encoder validates word capacity, transfer bounds, address overflow and alignment before mutation. It rejects overlapping copies and leaves the destination/count unchanged on failure. Maximum copy/fill size follows Linux's 1 GiB limit. Packet words have the intended DMA byte order directly only on little-endian hosts.

The encoder's 64-bit fence consists of ordered low/high 32-bit fence packets, not an atomic store. `sdma_queue.cpp` adds actual queue0 initialization, MMIO WPTR publication, copy/fill/IB submission, GCR ordering, 32-bit sequence fences, RPTR completion and verified shutdown. `HackyNaviSDMAQueue` uses real prepared DMA buffers bound by GART, and its copy test initializes distinct pages, submits a GPU copy and compares against independently generated expected bytes. There is no CPU-copy fallback. It leases Data/Writeback storage and retains failed hardware references. Pre-publication startup rejection can retire normally without asserting a reset. Interrupt-driven scheduling and execution on hardware remain pending.

### DisplayPort AUX

`dp_aux.cpp` implements DCN3.2's five physical AUX engines with GPIO pad selection, hardware arbitration, source-derived TX/RX tuning, explicit ATOM-selected HPD input and bounded packet/reply handling. It supports native DPCD and I2C-over-AUX operations, preserves MOT transactions, uses WRITE_STATUS for accepted pending writes and sends STOP before releasing an outstanding I2C chain. FIFO receive setup matches Linux's single initial read and three ordered writes. Display reference clocks, PHY power, connector routing and exclusion of firmware users must be established by the display owner before activation; AUX alone does not train a link or program scanout.

### HDMI DDC

`hdmi_ddc.cpp` owns DCN3.2's shared I2C controller for up to four repeated-start
messages. It selects the physical ATOM-routed GPIO pins, wakes the FIFO SRAM,
derives prescale from an explicit crystal clock and live hardware divisor,
and waits for actual DONE/error status. EDID segment/offset and SCDC helpers
use that transport. Release restores GPIO and the prior arbitration queue
policy; failures retain ownership when release cannot be proved. This module
does not enable the HDMI PHY, pixel clock, timing generator or scanout.

### Runtime display firmware and link ownership

`dmcub_firmware.cpp` validates the pinned DCN3.2 combined image, submits its
payload through PSP firmware type 51, and plans separate owned runtime windows
for VBIOS, inbox/outbox, trace and firmware state. Boot requires actual DAL boot
status and GPINT version responses. Shutdown uses STOP_FW acknowledgement,
PWAIT, reset and window disable before releasing memory. The inherited cold
stop path establishes those facts directly without inventing a loaded-image
owner. Cold admission additionally checks VCN/JPEG/LSDMA and IH/DWB/MCIF/MALL
state. Unknown or inaccessible state is rejected.

`HackyNaviDisplayController` holds an exclusive display-storage lease and
serializes its operations with the firmware and GART owners. It resolves board
routing from the selected, subsystem-matched ATOM image. `dio_link.cpp` programs
legacy DIO routing, source patterns and DMCUB transmitter commands.
`dp_training.cpp` uses the real AUX/DIO adapter to negotiate supported DPCD
capabilities and verify clock recovery, equalization, symbol lock and alignment.
It supports 8b/10b SST through HBR3; MST, LTTPR, DSC, FEC and UHBR are rejected.
Successful training leaves an idle trained link. Timing, pixel stream and
scanout programming are separate work.

`boot_display_stop.cpp` captures typed cold scanout ownership and blanks each
affected HUBP with bounded pre/post drain checks. Power-off pipes remain
untouched. It retires memory fetches, without claiming physical OTG/PHY power
down. `HackyNaviEngineController` connects fresh live discovery/ROM and these
admission checks to reservations, firmware/GART/graphics startup and DMCUB boot.
The coordinator's owner must exclude reset and power changes throughout its
lifetime; PCI service integration remains pending. Failed hardware writes seal
the owner and retain allocations until verified retirement or reset.

### Monitor timings and HDMI capabilities

`edid.hpp` / `.cpp` validate all captured blocks before decoding digital EDID
1.3/1.4. Acquisition length accounts for a first-extension HDMI Forum count
override, including overrides smaller than the base count. The parser exposes
progressive detailed timings and common 720p/1080p/2160p CTA timings, preserves
preferred/native flags, decodes SCDC/TMDS/deep-color capabilities and reconciles
4:2:0-only declarations across extensions. Unknown timings are counted rather
than synthesized. An unspecified maximum TMDS clock remains unspecified.

`hdmi_link.hpp` / `.cpp` implement native HDMI TMDS negotiation and activation
over actual DDC, DMCUB and DIO. The controller records connector link type,
DDC clock and SCDC capabilities for teardown. A disconnected sink can fail
SCDC cleanup while local source-disable and GPIO-release observations still
permit retirement. Uncertain local ownership retains the controller. HDMI
activation leaves the pixel FIFO off and AVMUTE asserted until the modeset
owner establishes timing, scanout and required information packets.

### Linux reference collector

`scripts/collect-linux.py` selects `1002:744c`, requiring an explicit BDF if ambiguous. It reads only existing sysfs text: PCI identity, assigned resource descriptions, driver binding, selected memory/link attributes, and the `ip_discovery` subtree.

Bounds are 8 KiB per attribute, 1 MiB of selected-device attribute reads, 2,048 files, 4,096 entries per enumerated tree, and depth 6. The selected PCI symlink must resolve inside the requested sysfs root; recursive discovery/attribute symlinks are not followed. Output must be a new file outside that root. Missing or unreadable optional attributes are recorded as warnings.

The collector does not enable/read ROM, open PCI configuration, map resources, or access debugfs. The reference is an interpretation already exported by the Linux driver, not the raw IP discovery binary or an atomic device snapshot.

## Bring-up sequence

| Milestone | Required evidence before advancing |
| --- | --- |
| Current driver foundations | Bounded ROM/discovery/firmware parsers, SDMA encoder and collector tests. Actual observer validation still pending. |
| Live IP discovery | The opt-in acquisition path exists in source. Validate its runtime operation and compare the target's Linux export, live binary and ROM metadata. |
| Memory and firmware | Establish IP versions, GPUVM layout, buffer ownership, firmware dependencies, interrupts, and completion/error reporting. |
| GPU execution | Verify a real SDMA copy/readback, then a minimal compute dispatch with completion and expected output. |
| Headless RADV | Implement the Darwin winsys and kernel interface; compare offscreen output against reference images. |
| Metal and display | Implement AIR/SPIR-V translation, resource/synchronization semantics, IOSurface sharing, DCN scanout, and WindowServer presentation. |

The complete service startup sequence and userspace acceleration remain unfinished. Real hardware protocols for firmware, GART, GC initialization and graphics/SDMA submission exist, but have not executed on this GPU or been connected to the PCI service's full lifetime. RADV supplies an AMD userspace backend, but its Linux DRM transport cannot be used unchanged. The intended eventual path is:

```text
Metal API → AIR/SPIR-V frontend → RADV/NIR/ACO → Darwin winsys
          → IOKit submission interface → AMD kernel driver → Navi 31
```

Display and IOSurface integration also require their own memory-sharing and synchronization work; a headless rendering result does not establish desktop acceleration.

### Output timing controller

`vpg.hpp` / `.cpp` implement DCN3.2 generic packet SRAM access for the five
legacy encoder instances and fifteen packet slots. The owner must hold the
associated packet scheduler disabled and establish the shared AFMT clock.
The transport checks the actual clock state, waits for VPG locks and pending
updates, wakes SRAM, writes the header and 32 payload bytes through the
auto-increment window, submits an immediate update and observes completion.
SRAM data is never read back through that auto-increment port. A failed write
retains the instance until independent retirement. Closing restores the saved
SRAM sleep controls after checking pending updates. `avi.cpp` constructs version-2 RGB/444 AVI payloads and checks range selection
against parsed CTA Video Capability Data Block bits. Conflicting duplicate
capability blocks are rejected. `hdmi_stream.cpp` connects this transport to AVI scheduling and the DIG FIFO:
prepare validates the live HDMI backend/frontend route, excludes other packet
senders, enables AFMT, uploads AVI and selects line 2. Start checks live OTG
master/clock status, performs the FIFO reset handshake, enables transmission,
and requires two frame advances without FIFO errors before clearing AVMUTE.
Stop asserts AVMUTE, disables FIFO and AVI, closes VPG and restores clock/DB
settings. Ambiguous failures retain ownership. The kernel display owner now binds these operations to its saved HDMI link
frontend and ROM-selected PHY, verifies matching pixel encoding, reserves
the OTG across paths, and retires the stream before PHY disable. Its display
lease cannot be released while a stream retains hardware. Shared PLL changes
are blocked while streams exist until a dedicated PLL lease manager is added.
The scanout pipeline and PCI-service activation remain unfinished.

`otg.hpp` / `.cpp` implement the DCN3.2 OTG/VTG/ODM timing sequence for
progressive single-pipe RGB. Register addresses are derived from validated DMU
discovery, including distinct OTG, VTG and ODM strides. Admission rejects live
timing generators, foreign locks/interrupts and another enabled OTG consuming
the selected OPP. Configuration programs timing, blanking, sync polarity and
caller-supplied global-sync parameters. Enable requires both clock status bits,
current master enable and an advancing hardware frame counter; shutdown
disconnects ODM inputs and verifies master-off and idle before clearing clock
controls. Any ambiguous write preserves retirement responsibility.

The timing controller does not establish a scanout surface or calculate display
bandwidth. Its owning modeset path still needs pixel PLL/DCCG, HUBP/DPP/MPC/OPP,
stream packets and native framebuffer integration. Tests exercise geometry,
all four pipe routes, each admission guard, bounded timeouts and transport
failure at every I/O position. These host tests do not establish visible output.

### Hardware access boundary

Linux helpers named as reads are not necessarily free of register writes. In the pinned Linux source, `amdgpu_device_vram_access(..., false)` can fall back to writes of `MM_INDEX` / `MM_INDEX_HI`; its aperture path can invalidate HDP. The active acquisition path therefore belongs to the opt-in PCI service, not the passive observer. It uses assigned BAR resources and documented bootstrap registers, not broad register scans. Device power/reset/removal behavior remains a runtime validation requirement.

## Source baseline

The architecture audit used these fixed revisions. They are research references, not vendored dependencies or completed ports.

| Project | Revision | Purpose |
| --- | --- | --- |
| [NVIDIA macOS driver](https://github.com/nullmoth/nvidia-macos-driver/tree/508f3d2496e78a9640d59128a56ddb5a53061606) | `508f3d2496e78a9640d59128a56ddb5a53061606` | Metal frontend/backend and macOS integration architecture. |
| [Linux](https://github.com/torvalds/linux/tree/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd) | `3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0` | AMD wire layouts, hardware initialization dependencies, and discovery/reference behavior. |
| [Mesa](https://gitlab.freedesktop.org/mesa/mesa/-/tree/35b085c4c067f198ce0b6437d94678e47b9c645f/src/amd/vulkan) | `35b085c4c067f198ce0b6437d94678e47b9c645f` | RADV/ACO and winsys boundaries. Repository URL: `https://gitlab.freedesktop.org/mesa/mesa.git`. |
| [libdrm 2.4.120](https://gitlab.freedesktop.org/mesa/drm/-/tree/75254bf2390c10644ffb35a90fc8f18f196f9f0c) | `75254bf2390c10644ffb35a90fc8f18f196f9f0c` | AMD GFX11 clear/copy machine code and dispatch definitions. |
| [Cobalt64](https://github.com/Almosst-DEV/Cobalt64/tree/d6c9aedf8e6526a39e349b5886cbc8c7ddf77f65) | `d6c9aedf8e6526a39e349b5886cbc8c7ddf77f65` | Navi 48 bring-up and RADV Darwin transport; see the [separate audit](cobalt64-audit.md). |
| [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK/tree/3f750085caa17ec3a7880f11c11bf4f48cd6a164) | `3f750085caa17ec3a7880f11c11bf4f48cd6a164` | Real kernel headers for compile-only checks on Windows; based on macOS 11 headers with modifications. |

The local audit also records these revisions in `.research/sources.json`; `.research/` is ignored by Git. No NVIDIA frontend or driver source was copied. Original project code is MIT licensed. The Cobalt64 PCI/MMIO adaptation preserves its MIT notice in source and [THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md). Other future upstream imports need component-level license review; the research references do not imply a common license across those projects.

Apple's [IOService notification declarations](https://github.com/apple-oss-distributions/xnu/blob/main/iokit/IOKit/IOService.h) and [implementation](https://github.com/apple-oss-distributions/xnu/blob/main/iokit/Kernel/IOService.cpp) informed the observer's ownership/lifetime design. Those source references do not replace compiling against the chosen macOS SDK or testing the intended runtime.

`scripts/cross-check-kext.py` compiles both services and shared code to x86_64 Mach-O objects using Zig and the pinned MacKernelSDK. Its report checks module definitions and both C++ initializer sections and records deployment metadata. The tested Zig linker rejects `-kext`; no linked bundle is produced. The native build additionally compiles and links with Apple Clang 17 and SDK 26.2 on macOS 15.8. Mach-O entry points/initializers validate and all imported symbols resolve against the host kernel collection. Read-only kmutil diagnostics report dependencies OK and build-folder ownership errors. SDK libkmod objects also produce a newer-deployment-version warning. Loading and target hardware behavior remain unverified.

The original offline corpus used eight AMD firmware files plus an unrelated Vega 10 discovery sample from [linux-firmware commit `afabaf773c4c2e2c841429933a6a084a5af4d14d`](https://gitlab.com/kernel-firmware/linux-firmware/-/tree/afabaf773c4c2e2c841429933a6a084a5af4d14d). The tracked `firmware/manifest.json` now separately specifies nine binaries embedded by kernel builds, including Navi31 RLC and DCN3.2 DMCUB. These support the current non-MES graphics/SDMA and display-firmware profile; video firmware is not included. Original binaries remain under their AMD license and are fetched/generated outside version control. No target-hardware firmware execution has occurred.
