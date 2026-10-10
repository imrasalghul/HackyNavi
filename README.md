# HackyNavi

HackyNavi is a Navi 31 / Radeon RX 7900 XTX driver under development for **macOS 15.8.1, x86_64**. Implemented code includes owned DMA/GART memory, PSP firmware loading and command transport, graphics firmware autoload, SMU wake controls, GC11 initialization and graphics/SDMA queues, DMCUB firmware boot, DisplayPort AUX/link training and HDMI DDC transport. Kernel adapters use real allocations and uncached MMIO; peripheral emulation is confined to host tests. **The PCI service does not yet run the complete startup sequence. Rendering, Metal integration and HDMI/DP modesetting remain unfinished, and no GPU execution or working display has been verified.**

## Implemented

| Component | Current behavior |
| --- | --- |
| Portable C++ parser | Finds PCI option-ROM images inside full dumps; validates image/table bounds; decodes the ATOM 2.2/2.3 common prefix, master data 2.1, VRAM 3.0, display 1.5, FirmwareInfo 3.4/3.5 and VRAM usage 2.1/2.2. |
| `hackynavi-rom` | Reads a ROM file up to 16 MiB and emits JSON with image identities, checksums, table metadata, alternative memory definitions, and display paths. |
| `hackynavi-discovery` | Validates an AMD discovery binary and emits its dies, IP versions and raw register-base encodings. Auxiliary tables are checked for bounds/checksums but remain opaque. |
| `hackynavi-firmware` | Validates common AMD firmware headers and selected GFX 2.0, SDMA 2.0 and PSP 2.0 layouts. Reports offsets and sizes without loading or authenticating firmware. |
| SDMA 6 encoder | Appends bounded copy, constant-fill and fence/trap packets to caller-owned CPU memory. It does not submit commands or allocate GPU memory. |
| SDMA 6 queue and kernel adapter | Programs queue0, publishes copy/fill/IB packets, waits for GPU-written fence/RPTR and verifies shutdown. Kernel adapter leases real GART DMA storage and provides a GPU copy/readback test with no CPU-copy fallback. Not activated by the PCI service yet. |
| Navi 31 IP layout | Requires matching GC/MMHUB/MP0/MP1/SDMA versions and resolves generated DWORD offsets through discovered bases, including both SDMA 6 engine-offset rules. |
| Memory layout | Decodes MMHUB 3 framebuffer registers, keeps MC/physical VRAM addresses separate from CPU BAR0, and validates VRAM-relative translations. |
| GMC11 page tables | Builds little-endian 4 KiB leaf entries and directory pointers in CPU memory. Enforces 48-bit VRAM and 44-bit system DMA address ranges, including invalid dummy entries with VALID/SYSTEM clear. |
| Kernel DMA buffer owner | Allocates owned kernel memory and generates device addresses through `IODMACommand`. Supports an associated mapper or a verified OS identity-DMA configuration. Compiled, but not yet called by the service or runtime-tested. |
| VRAM allocator | Allocates aligned ranges within a caller-established owned arena, excluding explicit reservations. Rejects stale and wrong-owner handles. Does not infer free VRAM from BAR size. |
| VRAM reservation builder | Combines firmware/usage metadata, complete live discovery regions, scanout and other supplied reservations; rounds outward, merges intervals and retains provenance. Reports missing evidence separately from malformed input. |
| DCN 3.2 boot-display capture | Resolves all four HUBPs and checks two register snapshots. Decodes supported linear RGB scanout into VRAM exclusion intervals; rejects unsupported fetching/address modes. Does not establish free VRAM or program displays. |
| CP/SDMA bootstrap stop | Halts and verifies the selected CP architecture and both SDMA6 engines with bounded polls and failure reporting. Rejects prior-driver and unsupported queue state. Completion covers these engines only. |
| MODE1 reset and ATOM ASIC initialization | Native MODE1 adapter captures real PCI state, observes the 500ms reset blackout and verifies restoration with bus mastering off. A bounded ATOM interpreter executes the board's actual ASIC_Init bytecode through MMIO and checks its hardware completion response separately from the firmware's completion marker. Service recovery integration remains pending. |
| Embedded AMD firmware | Builds immutable resources from nine size/SHA256-pinned upstream binaries, preserving the AMD license. Missing or modified inputs fail the build. No userspace firmware service is needed to obtain the bytes. |
| PSP13 bootloader and KM ring | Uploads owned VRAM, verifies readback/HDP publication, executes bootloader commands and ring commands, and checks real mailbox/fence/response status. Includes TOC-derived TMR setup and destruction. |
| Graphics firmware loader | Validates all seven engine firmware files, sends the source-ordered PSP loads, starts RLC autoload and polls CP/bootload status. Exact revision C8 profile with MES disabled. |
| Kernel firmware controller | Shares GART's allocator and mutex, loads embedded SOS, creates the PSP ring/TMR, loads engine firmware and the register list, and retains resources across uncertain hardware failures. Not called by the PCI service yet. |
| SMU13 power control | Queries real firmware versions, sends DisallowGfxOff, checks mailbox completion and verifies IMU reset release, CP idle and RLC bootload. Connected to the kernel firmware controller. |
| GC11 initialization | Programs RS64 per-pipe start PCs, pipe reset pulses, golden/constants state, full AMD clear-state stream and owned RLC CSIB/SRM state. Leaves CP halted for queue setup. |
| Graphics queue and kernel owner | Uses real GART DMA storage, a 16KiB ring and separate 16KiB IB, starts pipe0 and waits for GPU-written clear-state fence/RPTR. Supports trusted kernel IBs and CP writes; a canary-page test checks GPU-written results. Not called by the PCI service yet. |
| GFX11 shader dispatch | Uploads AMD clear/copy machine code to owned VRAM with readback, programs compute state and buffer descriptors, dispatches through the graphics queue, and waits for cache writeback and a GPU fence. Engine startup requires two real dispatches and comparison of 10,240 output/guard words. This verification code has not executed on the GPU yet. |
| DCN3.2 DisplayPort AUX | Implements GPIO/AUX ownership, native DPCD and I2C-over-AUX packets, reply validation/retries and STOP/release. |
| DMCUB firmware and kernel display owner | Loads the pinned DCN3.2 image through PSP, maps owned runtime regions, boots DAL firmware and checks actual GPINT/version replies. Serializes AUX, DDC, DIO and inbox commands; retains memory across ambiguous failures. |
| DIO and DisplayPort training | Programs source routing, lane count and training patterns; negotiates supported sink capabilities and verifies clock recovery, equalization, symbol lock and alignment. Uses real AUX/DMCUB/DIO adapters. A trained idle link does not establish scanout or visible output. |
| Runtime ROM and cold takeover | Acquires the selected live SMUIO ROM image, validates board binding and checks stopped auxiliary engines. Captures inherited boot scanout and implements bounded HUBP blank/drain retirement. |
| Kernel engine coordinator | Connects fresh discovery/ROM, reservations, GART, PSP, graphics verification and DMCUB startup. Requires the PCI owner to supply power/reset exclusion; service activation is pending. |
| DCN3.2 HDMI DDC and TMDS | Implements shared physical I2C arbitration/FIFO, pin selection, clock-derived prescale, EDID/SCDC transactions and source PHY activation through DMCUB/DIO. Negotiates scrambling/clock ratio against explicit sink and board limits. Native controller tracks HDMI versus DP teardown. Pixel timing, AVI packets and scanout remain separate work. |
| EDID modes and capabilities | Validates complete digital EDID captures, checks every block checksum and extension-count override, decodes progressive detailed timings/common CTA modes and HDMI capabilities, and excludes YCbCr 4:2:0-only modes from RGB output. |
| DCN3.2 output timing | Programs progressive single-pipe RGB OTG/VTG/ODM timing, verifies master enable and frame-counter progress, and disables timing clocks after bounded master-off/busy checks. Pixel PLL, scanout pipeline and native modeset integration remain unfinished. |
| Kernel GART storage | Owns fault-page, data, writeback and display DMA buffers and builds a CPU page-table image with invalid guard pages. The fault-page address is backed by a real allocation. No VRAM upload or hardware reference is created by storage allocation alone. |
| GART protocol | Builds and executes bounded GC 11 / MMHUB 3 VMID0 register plans, with mandatory HDP flushing and engine 17 invalidation. Simulated-MMIO coverage; not yet called by the service. |
| HDP protocol | Resolves NBIO 4.3 remap controls from discovery, configures the remap hole, and implements the CPU memory flush with posting read. Simulated-MMIO coverage; not yet called by the service. |
| Owned VRAM transfers | Uses the MM_INDEX/HI/DATA protocol to upload an owned allocation and verify every DWORD in a separate readback pass. Rejects stale handles and out-of-allocation ranges. |
| Kernel GART controller | Connects real DMA buffers, private VRAM allocation, upload/readback, HDP setup and separate MMHUB/GFXHUB activation through uncached BAR5 MMIO. Retains resources across partial hardware failure. Compiled; not yet activated by the PCI service or hardware-tested. |
| `collect-linux.py` | Collects existing sysfs identity, resource descriptions, memory/link attributes, driver binding, and exported IP discovery for `1002:744c`. |
| `HackyNavi.kext` source | Attaches to `IOResources`, observes matching PCI devices, and publishes bounded historical snapshots. Reads PCI configuration and existing ROM registry properties. |
| `HackyNaviDevice` source | Opt-in PCI service for `1002:744c`. Joins IOKit power management, captures discovery/MMHUB memory state on power-up, and gates MMIO and invalidates snapshots on power-down, shutdown and teardown. Hardware operation remains untested. |

The observer does not claim the GPU, map BARs, size-probe BARs, load firmware, execute ATOM commands, change power state, or expose an accelerator/user client. Its `ConfiguredBARs` addresses are configured values; `BARSizeStatus` remains `NotProbed`. Missing ROM properties are reported as missing, rather than fetched through hardware.

## Build and inspect a ROM

The host tools require Python 3.9+ and `clang++`, `g++`, or Zig. From the repository root:

```powershell
python scripts/build-host.py --rom "path/to/Navi 31.rom"
```

Use `python3` where that is the Python command. Supply `--compiler PATH_TO_EXECUTABLE` if autodetection is unsuitable. The script builds the inspection tools and test executables, runs the C++ and Python tests, and writes the optional inspection report to `build/host/rom-report.json`. On Windows, executables have an `.exe` suffix.

The original ROM is opened only for reading. Exit status 0 means at least one structurally valid PCI image was found; individual JSON status fields still need checking for malformed or unsupported ATOM tables. Checksums are diagnostic and do not establish firmware authenticity.

CMake is also supported:

```sh
cmake -S . -B build/cmake
cmake --build build/cmake --config Release
cmake -E chdir build/cmake ctest --output-on-failure -C Release
python3 -m unittest discover -s tests -p "test_*.py"
```

The CMake build covers the portable tools, not the kext. Clang/GCC builds on macOS or Linux can additionally configure `-DHACKYNAVI_SANITIZERS=ON`.

## Inspect discovery and firmware files

After the host build, these commands inspect separately acquired inputs:

```sh
build/host/hackynavi-discovery discovery.bin
build/host/hackynavi-discovery --scan "Navi 31.rom"
build/host/hackynavi-firmware gfx gc_11_0_0_pfp.bin
build/host/hackynavi-firmware sdma sdma_6_0_0.bin
build/host/hackynavi-firmware psp psp_13_0_0_sos.bin
build/host/hackynavi-firmware common smu_13_0_0.bin
```

Default discovery input starts at the binary signature, without a PSP prefix. `--scan` searches a larger ROM or binary for every signature and reports candidate offsets and parser results, including invalid candidates. It limits work to 128 candidates and 4 MiB of cumulative parser-input windows; a limit returns exit 4 with `scan_complete: false`. A complete scan returns 0 if any candidate validates, otherwise 3. The collector's sysfs JSON is a different format. Binary header versions 0/1/2 and IP table versions 1–4 are supported, including raw 64-bit base-address fields. Those fields are not CPU physical addresses and must not be mapped directly.

Firmware family selection is explicit: its common header has no identifying signature. Typed decoders currently support exact version 2.0 only; `common` reports the shared metadata without interpreting family-specific fields. Stored CRC32 is reported but not verified. A successful parse establishes bounded structure, not authenticity, GPU compatibility, or readiness for PSP loading. All inspectors limit files to 16 MiB. The discovery/firmware tools return 3 for rejected or unsupported structures, and 2 for input errors.

## Collect a Linux reference

Run this on Linux with the target GPU present:

```sh
python3 scripts/collect-linux.py --output navi31-linux.json
```

If multiple `1002:744c` devices exist, select one with `--device DOMAIN:BUS:SLOT.FUNCTION`, for example `--device 0000:03:00.0`. Use the actual BDF from that machine. Existing output files are refused. Optional unavailable attributes become warnings; an unbound device is explicitly distinguished from one using `amdgpu`.

The collector reads existing sysfs text only. It does not read PCI `config`, ROM contents, `resourceN`, or debugfs, and does not write to sysfs. Its IP discovery output is Linux's exported interpretation, not a raw discovery firmware binary. The snapshot is not atomic.

## Build the macOS services

On a Mac with Xcode selected and a macOS SDK version 15 or newer containing `Kernel.framework` IOKit/PCI headers and the kernel support archives:

```sh
python3 scripts/fetch-firmware.py
bash scripts/build-kext.sh
```

This produces an unsigned `build/macos/HackyNavi.kext` for **x86_64**, with deployment baseline 15.0 and intended runtime 15.8.1. An Apple silicon Mac can be the build host; the resulting kext is for the x86_64 macOS kernel. The script only builds and checks the artifact. There is no installer.

An existing SSH-accessible Mac can build from a Windows checkout with `python scripts/build-remote.py --host user@mac`. This helper requires Python 3.12+ locally, sends only project sources and pinned firmware, builds in a unique temporary directory, copies the bundle/report into `build/macos-native`, and removes remote staging even when compilation fails. It checks available space before uploading and installs no tools or dependencies. The native build selects the SDK kernel C++ archive or Clang's `libclang_rt.cc_kext.a`, whichever is available.

`fetch-firmware.py` downloads only the exact upstream files and license in `firmware/manifest.json`. Existing mismatched files are rejected. An existing matching linux-firmware directory can instead be selected with `HACKYNAVI_FIRMWARE_DIR`; the cross checker uses `--firmware-dir`. Generated arrays, downloaded firmware and build outputs remain outside version control. The firmware retains its separate AMD license; it is not relicensed under this project's MIT license.

Once the observer has been built and loaded in a separately prepared test environment, its registry snapshots can be read with:

```sh
ioreg -r -c HackyNavi -l -w0
```

An empty result does not diagnose GPU support: the kext may not be loaded or matched. `AccelerationImplemented` is always false in this milestone.

The separate `HackyNaviDevice` personality requires the boot argument `-hackynavi-device` before it can match. This experimental service performs **active hardware access**: it maps assigned BAR5 and writes the VRAM read selectors. Acquisition runs in the IOKit power-up callback after its parent is powered; sleep, shutdown and teardown block access and invalidate snapshots. After validating the required IP versions, it reads three MMHUB framebuffer registers twice to detect observed changes and compares BAR0 configuration with its assigned resource. It does not enable bus mastering, program display registers, reset the GPU, map BAR0, or submit commands. Its discovery/IP and memory-layout reports are exposed through `ioreg -r -c HackyNaviDevice -l -w0`. Matching may finish before capture; `SnapshotStatus` and `SnapshotValid` report that outcome. This repository has not yet produced a hardware-tested build; the boot argument is a description of the implementation, not a tested installation procedure.

For a compile-only check on Windows, an existing [Acidanthera MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) checkout and Zig can be used:

```powershell
python scripts/cross-check-kext.py --sdk .research/MacKernelSDK --probe-link
```

This compiles both services and their shared source files into x86_64 Mach-O objects and checks their format, module symbols and initializer sections. The report records SDK revision, compiler version, commands and hashes. Tested with MacKernelSDK `3f750085caa17ec3a7880f11c11bf4f48cd6a164` and Zig 0.17.0. MacKernelSDK uses macOS 11 headers with project modifications; selecting a 15.0 deployment target does not turn it into a macOS 15 SDK. Zig currently rejects `-kext`, so the optional link probe records that failure. This path produces **no loadable kext** and does not replace the native SDK build above.

## Validation and target machine

On the Windows development host, Zig 0.17 built the host tools and the ROM C++ suite completed **5,687 assertions with zero failures**, including 4,097 guarded truncations and 5,000 mutated fixtures. The optional supplied-ROM run passed **5,703 assertions**. The Python suite ran 24 tests: **22 passed and 2 symlink tests were skipped** because the host lacked symlink privileges. Those tests remain available on Linux. This run set `HACKYNAVI_ROM` to the supplied ROM to enable the optional scan/hash check; that check skips when no ROM is provided. The supplied 2 MiB ROM yielded four PCI images; both legacy images decoded ATOM, VRAM, display and reservation tables successfully.

Additional suites passed **6,240 discovery checks**, **39,358 firmware checks**, and **1,980 SDMA packet checks**. The acquisition suite passed **27,804 assertions** using simulated MMIO, including failures at every read/write step in default and scratch acquisition. Discovery and firmware tests include guarded truncations and deterministic mutations. An optional firmware run against eight official AMD files passed 39,389 checks; a separate real Vega 10 discovery sample parsed 22 IP records. These samples establish format evidence, not operation on Navi 31.

The supplied ROM also contains two identical 1,756-byte discovery payloads at offsets `0xB5D00` and `0x195D00`. Both validate as binary version 1.4 / IP table version 3 with 61 IP records. They describe GC 11.0.0, SDMA 6.0.0, MP0/MP1 13.0.0, and DMU/DCN 3.2.0, among other blocks. This is target ROM evidence; live discovery, enabled topology and running firmware remain unverified. See [architecture.md](docs/architecture.md#target-rom-discovery-evidence) for hashes and scope.

Memory work passes **1,618 register-layout assertions**, **59 memory-layout checks**, **58,536 GPUVM assertions**, and **35,364 VRAM allocator assertions**. The allocator is also checked against an independent page bitmap over 10,000 operations. The optional extracted target discovery payload increases register-layout coverage to 1,627 passing assertions. The HDP suite passes **895 assertions**, or **899** with that payload. GART's **2,229 assertions** cover register values, ordering, injected failures, timeouts, semaphore cleanup and verified disable. The VRAM transfer suite covers maximum-size uploads, selector boundaries, stale handles, aliases and partial failures using simulated MMIO. All 35 current C++ suites have passed through CMake/CTest on this Windows host using Zig and explicit Zig archiver rules. The DMA owner, GART storage and controllers compile in the native kext; actual allocation, mapper behavior and DMA remain untested.

**A native x86_64 kext has compiled and linked on macOS 15.8 (24H22) using Apple Clang 17 and SDK 26.2.** Its Mach-O bundle, entry points and C++ initializer sections validate; every imported kernel symbol resolved against that Mac's kernel collection. The linker reports that the SDK's `libkmod.a` objects were built for a newer OS than the selected 15.0 baseline. Read-only `kmutil` diagnostics report dependency success and ordinary-user ownership errors for the build folder. This is an unsigned build, not a loading or GPU-execution result. Detailed source hashes, artifact metadata and diagnostic output are retained in the local build report.

The repository includes a GitHub Actions workflow for Linux sanitizer tests and an unsigned x86_64 kext build on a macOS runner. CI results are separate from the local and SSH build evidence above.

The reported target is:

| Item | Reported configuration |
| --- | --- |
| CPU | AMD Ryzen 9 7900X, 12 cores / 24 threads |
| Motherboard | MSI PRO B650-P WIFI, MS-7D78; BIOS 1.L0 |
| System memory | 32 GB, two 16 GB modules, configured at 4800 MT/s |
| Target GPU | RX 7900 XTX, `1002:744c`, revision `c8`, subsystem `148c:2422` |
| Separate integrated GPU | `1002:164e`; outside this observer's target |

“Intel macOS” here describes the **x86_64 kernel ABI**, not a requirement for an Intel-branded processor. A Ryzen system needs a separate compatible CPU/platform boot configuration. [AMD_Vanilla](https://github.com/AMD-OSX/AMD_Vanilla) documents AMD kernel patches, including Zen 4 considerations; [WhateverGreen](https://github.com/acidanthera/WhateverGreen) documents its graphics patching scope. These are background references, not validation of this exact board, BIOS, macOS point release, or Navi 31 acceleration.

## Next work and sources

The next integration must connect the engine coordinator to the PCI service with reset/POST and power management. GC11 initialization, kernel compute dispatch, DisplayPort link training and HDMI TMDS activation now exist; general rendering, interrupts, RADV integration, Metal translation, IOSurface sharing, display clocks and HDMI/DP modesetting remain required for the requested driver. The GPU copy/write/readback implementations have not executed on hardware. See [architecture and implementation boundaries](docs/architecture.md).

The repository's original code is [MIT licensed](LICENSE). NVIDIA's project was studied for architecture; its driver/frontend implementation was not copied. Linux's MIT-licensed ATOM definitions informed the wire layouts. The audited upstream revisions are recorded in [architecture.md](docs/architecture.md#source-baseline). Local `.research/` checkouts, downloaded tools, ROM dumps, and generated reports are excluded from version control.

[Cobalt64's Navi 48 implementation was also audited](docs/cobalt64-audit.md). Its PCI mapping/MMIO and VRAM read-selector code informed the implemented Navi 31 discovery path; the [upstream MIT notice is preserved](THIRD-PARTY-NOTICES.md). Its RADV Darwin transport remains a reference for later work. The RDNA 4 initialization paths and fixed `gfx1201` device model require separate adaptation for Navi 31.
