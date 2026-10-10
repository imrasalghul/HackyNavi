# Cobalt64 applicability to HackyNavi

Inspected [Almosst-DEV/Cobalt64 at `d6c9aedf8e6526a39e349b5886cbc8c7ddf77f65`](https://github.com/Almosst-DEV/Cobalt64/tree/d6c9aedf8e6526a39e349b5886cbc8c7ddf77f65). Its builds, installer scripts and hardware operations were not executed. Selected PCI/MMIO and VRAM read-selector code has since been adapted into HackyNavi, as detailed below.

## What the source establishes

The project is a directly relevant AMD reference: an IOKit kernel driver, a Darwin port of RADV, and a Metal frontend that submits Vulkan work. This complements the earlier NVIDIA architecture reference and gives us existing AMD-specific transport and initialization designs to study.

Its current target is Navi 48 / RDNA 4. The PCI probe explicitly accepts `1002:7550` and `1002:7551`, not the user's `1002:744c`. The [README](https://github.com/Almosst-DEV/Cobalt64/blob/d6c9aedf8e6526a39e349b5886cbc8c7ddf77f65/README.md) describes testing on one RX 9070 XT machine running macOS 26.6.2, with RDNA 3 left for future work. Those are author reports, not results reproduced here.

| Evidence in the pinned tree | Implication for Navi 31 |
| --- | --- |
| `src/navi48-bringup/src/Navi48Bringup.cpp:7335` rejects devices outside the Navi 48 IDs. | A PCI ID edit alone cannot provide the missing generation-specific initialization. |
| `src/navi48-bringup/src/amd/amdgpu_init.cpp:468` initializes SDMA; the subsequent path issues a 4 KiB VRAM copy and checks readback. | Use actual copy/readback as the execution milestone; packet construction or successful firmware parsing is insufficient. |
| `mesa-patches/0002-...patch` implements IOKit calls for buffer allocation, CPU mapping, GPU VA mapping, submission and completion. | Adapt this transport design once HackyNavi implements the corresponding kernel operations. A new transport need not be designed from scratch. |
| `src/navi48-bringup/src/Navi48NativeABI.h` shares versioned structures/selectors with the Mesa backend. | Keep one tested ABI definition and explicit version/capability negotiation. Do not claim compatibility with N48N before its semantics are implemented. |
| `mesa-patches/0001-...patch` seeds a fixed `gfx1201` device; later patches add live budgets and `GB_ADDR_CONFIG`, but retain generation-specific expectations. | Replace the device model with verified Navi 31 topology, firmware and feature data. Service renaming and bypassing the address-configuration check are not a port. |
| `tools/native/navi48metal/Navi48Device.m:4515` contains a real `vkQueueSubmit` path. | The frontend has substantial implementation worth studying after the kernel and RADV execution path work. |

## Boundaries that must survive any reuse

Navi 31 needs its own GC 11, PSP/SMU 13, SDMA 6 and DCN 3.2 paths. Cobalt64's corresponding Navi 48 code and firmware selections target later hardware generations. Register programming, firmware sequencing, memory topology and display setup must be derived from the appropriate Linux implementations and checked on the target.

The native host-memory import path also assumes CPU physical addresses equal device bus addresses with no IOMMU translation (`native_s1c.cpp:1305`). HackyNavi must establish DMA mapping and ownership for its actual platform rather than inherit that assumption. The patched Mesa base is `f5cb8ee032adabef599ae892ec4e42d796b84da9`, which differs from HackyNavi's original Mesa research pin.

Queue and synchronization capabilities also need explicit limits. The native submit validator accepts only GFX/instance 0/ring 0 (`native_s1c_pure.h:268`), while the Mesa model contains fixed compute-IP information. Submission dependencies rely on single-ring assumptions, and sync-object/sync-file FD import/export return `ENOSYS` in patch 0002. Reusing the transport does not establish complete Vulkan synchronization or queue support.

Metal success needs stronger evidence than creating a pipeline or showing a desktop. The pinned `Navi48Device.m` can substitute magenta render shaders after a cache/translation miss (around line 3877), and a no-op compute pipeline under admitted fallback conditions (around line 6042). `n48_gate.h:145` includes WindowServer and admitted applications in that policy. These are explicit development fallbacks; they cannot establish correct rendering or compute output for an arbitrary application. HackyNavi validation must distinguish fallback activity from successful shader translation and execution.

The Metal build also assumes externally prepared dependencies and shader-cache files. Its script references `spvcache/*.spv` and `*.meta.json`, while that cache is absent from the tracked checkout. The repository is not a self-contained, verified macOS 15.8.1 build here.

## Decisions

1. Use the MacKernelSDK build approach to pursue real kernel-header compilation on the Windows development host; keep compile/link evidence separate from macOS loading and hardware validation.
2. Keep the current bounded parsers and SDMA 6 encoder as Navi 31 foundations. Establish device ownership, GPU memory and firmware initialization before any hardware submission.
3. Adapt the Darwin IOKit transport and synchronization design after the kernel provides real buffer, VM and submission operations. Replace fixed `gfx1201` identity/capabilities with a verified `gfx1100` model.
4. Evaluate the Metal frontend separately against macOS 15.8.1. Require correct GPU readback and explicit unsupported-feature behavior; do not count placeholder pipelines as supported functionality.

The main project is MIT licensed, with separate third-party notices. Its translator changes are LGPL-3.0 and firmware has AMD's separate license. The current PCI/MMIO adaptation retains the applicable MIT notice in the source and [THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md). Translator and firmware code is not included.

The build adaptation now compiles all sixteen kernel source files into x86_64 Mach-O objects against actual MacKernelSDK headers, including both IOKit services, the GART controller and the acquisition, memory and parser code. Zig's linker rejects `-kext`, so this remains an object compilation check, not a loadable driver or a macOS 15.8.1 runtime result.

## Implemented adaptation

`HackyNaviDevice.cpp` adapts the assigned BAR5 mapping and register access approach from `Navi48Bringup.cpp:122–165`. It adds a Navi 31 PCI match, explicit opt-in, provider ownership, uncached mapping, aligned subtraction-based bounds checks, and complete failure cleanup. The passive observer remains a separate service.

`device_discovery.cpp` adapts Cobalt64's `MM_INDEX_HI`, `MM_INDEX`, `MM_DATA` read protocol. Bootstrap register numbers, ready polling, VRAM size, default discovery location and driver-scratch fallback are checked against the pinned Linux implementation. A callback interface permits fake-MMIO tests without touching hardware. It caps polling, refuses unsupported system-memory discovery, validates all VRAM ranges and caller capacity, and reports incomplete acquisition. Parsing occurs only after a complete capture.

These helpers write selector registers, so they are active device operations even though the payload is only read. Selectors remain changed; neither reference establishes a restoration protocol. The caller serializes the complete operation. The default location is VRAM end minus 64 KiB, with Linux's 10 KiB capture region; a valid scratch descriptor may select another bounded VRAM region. No Navi 48 engine initialization sequence has been transplanted.

`vram_transfer.cpp` also adapts the write protocol from Cobalt64's `amd/amdgpu_regs.h`, checked against Linux's matching Navi 31 MM window. It confines transfers to live allocator handles, bounds each transfer, stages DWORDs explicitly as little-endian data, and verifies the full upload in a separate readback pass. `HackyNaviGARTController` connects it to actual kernel allocations and volatile BAR5 accesses, followed by Navi 31 HDP and hub programming. Its controller lifetime preserves memory after partial hardware failure; runtime activation still requires device ownership and initialization prerequisites.

The acquisition suite passes 27,804 assertions with simulated MMIO. It checks 24 GiB addressing, the 2 GiB selector boundary, default/scratch regions, short buffers, odd final bytes, polling limits, and injected failure at every transaction in complete 1,756-byte capture sequences. All twelve portable C++ suites pass CMake/CTest, including subsequent register and memory work. Independent source review found no further concrete defect in the PCI service. These results do not establish live hardware access or macOS loading.

The memory components use Navi 31 definitions directly: discovery-based register resolution, MMHUB 3 framebuffer decoding, GMC11 PTE/PDE encoding, an IOKit DMA buffer owner, bounded VRAM allocation, guarded GART storage, and GFXHUB 3 / MMHUB 3 / NBIO 4.3 register protocols. The DMA owner obtains addresses through `IODMACommand` and an explicitly resolved platform mapping policy. The new hardware programming helpers are compiled and tested with simulated MMIO, but are not yet invoked by the service.

Cobalt64's `gmc_v12_0.cpp` fixed firmware/PSP/GART offsets and allocator start at 24 MiB do not establish free space on Navi 31. Both supplied ROM images declare a 4 MiB firmware tail reservation even though their flash VRAM-usage tables contain zeros. Live scanout, firmware, discovery, training and driver-owned regions still require accounting. Navi 31's PSP 13 path also allocates its TMR separately; Cobalt64's PSP 14 reservation behavior is not applicable. The allocator therefore accepts an explicitly established arena and reservations, and the service continues to report `ReservationsNotEstablished` until runtime ownership exists.
