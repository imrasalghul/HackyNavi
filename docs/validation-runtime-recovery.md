# Runtime failure, SMU recovery and client VM initialization

Integrated 2026-10-11 in `build/macos-runtime-recovery-final`. The native kext
links all 88 translation units and resolves all 406 imported kernel symbols.
All 228 recorded source hashes matched after export; native/cross source lists
agree. This build has not loaded or executed on the target GPU. The diagnostic
capture ZIP remains the separate immutable `1e4040229efa` artifact.

## Actual runtime failure containment

FunctionGroup now accepts an exposed-workload failure from the exact still-held
Operation. Its private address, thread, issued generation, instance and retained
graphics/open-owner identities must match. The scope may have lost hardware
validity during the failed operation; the new method grants failure reporting
only, never further hardware access.

ClientSession invokes it after genuine queue/VM exposure or failure of actual
completed-job readback/retirement. Capture-validation and allocation failures
remain local. The method revokes admission and schedules the existing worker
and independent watchdog without joining while Session/Operation locks are
held. Callback and runtime origins share reason merging and the terminal-event
versus PCI-recovery exclusion. The worker drains the real operation gate before
calling Lifecycle. Failed jobs retain their original errors and publish no
completion sequence. Failed recovery still follows the existing fail-stop
policy; this is not a claim of seamless device restart.

Focused evidence: `build/native-runtime-failure-retirement/report.json` and
`review.json`. Two actual Apple Kernel SDK objects compiled; the 584 KiB stage
was removed. Source review found no concrete defect. A simulated IOKit authority
test was not substituted for these real native bindings.

## Terminal SMU table reconciliation before reset

The clock companion records each potentially delivered mailbox phase and the
last actual message serial. A fault permits one terminal reconciliation attempt
under a distinct native pre-reset RetirementScope. A known prior terminal reply
only settles the transport. It does not turn the failed transfer into success.

Five fresh commands must ACK: firmware version, interface, both halves of the
same retained VRAM address and a new table5 transfer. Exact allocation identity,
ordered 392-byte readback and a final scope/deadline check precede the table seal.
Unknown replies, zero forever, changed firmware, unavailable hardware, changed
allocation or any new ambiguity retain the allocation. Normal readiness stays
revoked; no generic clear-fault or pointer-clear assertion is introduced.

The private Lifecycle -> Engine -> Firmware path runs after actual
`enterRecovery()` and before the prepared ResetController's first MODE1.
Engine allows it once. Healthy completed tables use the existing terminal
seal; ambiguous tables use reconciliation. Outstanding SOC/Fabric ranges reject
this path before new table work. Only then can the separate MODE1/ASIC_Init and
typed reset proof retire other owners. See [the source contract](smu-table-recovery.md).

Local and native Apple Clang ASan/UBSan runs each passed 127,358 checks, including
partial address publication, response-clear/argument failure, late ACK, rejected
commands, all access failures/revocations and deadlines. Independent review
found no concrete defect. Normal metrics/range startup activation remains absent.

## Client VM shader memory configuration

GC initialization now selects graphics VMID1..7, writes and independently reads
back `SH_MEM_CONFIG=0xc00c` and `SH_MEM_BASES=0x20001000`, then restores selector0.
The private/shared aperture addresses come from pinned Linux GMC11 policy and
cannot overlap the driver's admitted low47 GPU VA range. VMID0 bases and
VMID8..15 shader-memory banks are unchanged. All nonzero contexts must still be
disabled before programming. This does not activate a context, allocate scratch
or LDS, or admit general ISA.

Local ASan/UBSan passed 17,403 initialization checks and 5,156 impacted queue
checks; native Apple Clang sanitizers also passed 17,403 initialization checks.
The model banks the registers by VMID and verifies every new write/read failure,
dirty initial bank, readback corruption and final selector restoration. See
`build/gfx-init-shmem-validation.json` and [admission design](compute-product-admission.md).

## Combined verification and exact artifacts

`build/native-recovery-shmem-final/report.json` contains the native sanitizer
runs plus actual FirmwareClocks/Lifecycle kernel compilation. Every captured
source hash matched; the 3,864 KiB stage was removed. Fresh CMake compilation
passed all 3 selected suites (`smu-clock`, `gfx-init`, `gfx-queue`) in 71.26 seconds.
This is targeted validation, not a new claim about every unrelated test.

The full native build used Intel macOS 15.8/24H22, Clang 17, SDK 26.2 and deployment
baseline 15.0. Its 4,795,820-byte source snapshot and 9,568 KiB temporary stage are
recorded; the stage was removed. No-load kmutil returned zero and resolved all
imports, while reporting build-folder ownership 501:20 and SDK libkmod deployment
warnings. Those diagnostics remain in the log; loading was not attempted.

| Artifact | SHA-256 / identifier |
| --- | --- |
| Source archive | `b2d8cb95c19b5fe7e2865022c43e2a9e87a2f89600ac249ba9333c5fde2b42e5` |
| Artifact archive | `d760589f9f9a3f74b7bc0f81a55e3a63c8dad93b1d886e129b51fa1aa5d35a9f` |
| Executable | `5624a26ba6076a9d18cb0f1c75535fc04c3cb53e89f00eabef10a52f6468bb61` |
| Mach-O UUID | `2FC2F154-8126-32D7-8C65-649BFF49EA3C` |
| Build report | `69f5bab73020d3371fc05c58bfe0dd7197806b2491f12a70de4ca6f5b6fb6355` |

Next integration is machine-code validation and immutable multi-region native
submission for actual compiler products, then persistent resources and the
Metal frontend. Controlled modeset/IOSurface and target execution remain
unproved. Compiler products, source checks and native links are not Metal
acceleration evidence.
