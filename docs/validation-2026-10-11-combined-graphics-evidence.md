# Combined graphics-attempt preparation

The next issued target test must attempt actual graphics and collect the
necessary evidence in that same session. No new capture-only package or target
reboot has been requested. This change prepares that run; it does not remove the
known inherited-framebuffer startup rejection or establish GPU execution.

## Implemented and built

- The engine route preserves its original display/context/console observations
  before lifecycle preparation, then independently performs real startup checks.
- Paired-function creation and power admission retain their actual outcomes,
  including factory state before failure cleanup.
- Reset/ATOM preparation retains attempted/completed steps and available PCI,
  SOS and scratch results before Engine startup.
- Training, LSDMA, CP/SDMA and DMCUB preflight observations and a bounded stage
  journal survive later failures. BootDisplay and startup cold-DMA read journals
  preserve existing successful/partial reads without extra hardware accesses.
- Metal loader/ABI rejection reasons are retained. The four public test tools
  flush progress before potentially failing operations and preserve real
  compute/pixel comparisons. No software output substitutes for GPU readback.
- The target runner verifies the eight executable/metallib inputs, captures
  before/after logs and per-test state, stops on a failed test or changed owner,
  and records skipped work. Collector completion, individual query success,
  graphics outcome and final runner exit are separate fields.

## Verification and immutable artifacts

`build/combined-graphics-integration-1/report.json`, SHA-256
`1ee12b15b1ed351abd5607ac8b826eb4d393d0d4b1b056ea91cfd8a0bc4cbe88`,
binds the current sources to the following frozen builds. All 291 core input
files, 63 frontend source files and six archived frontend inputs match. The 18
retained 3fa target files and the issued 3fa ZIP remain unchanged.

| Evidence | Result |
| --- | --- |
| `build/macos-combined-graphics-evidence-2/build-report.json` | Full native compile/link: 113 core units and two late-accelerator units; all 413 core imports resolve from build-host BootKC. Finished 2026-10-11 21:09:07 UTC. |
| `build/graphics-progress-frontend-native-1/report.json` | 22 frontend units and four public test tools compile/link on macOS. No Metal device or workload created. |
| `build/metal-graphics-evidence-development-package-1/validation.json` | Development archive has 137 verified files and 15 native images; 24 corrupt variants rejected. Not installed or issued for testing. |
| `build/graphics-runner-native-syntax-2/report.json` | Both scripts pass actual macOS `/bin/sh -n`; no script commands/GPU work executed. |
| `build/read-capture-local-1/report.json` | Strict/UBSan protocol regressions and ordered-read fidelity/failure tests pass; negative controls fail as intended. |
| Focused real-header reports | Engine, Device, Reset, Lifecycle, ATOM and FunctionGroup compile with the 2 KiB frame check. |

The native build report is SHA-256
`db906e4ab71ac495803a76ec260757bfdefd3cccdb39e656e166ba0851404c88`.
Its internal core is `d0c2af8538b0d26b591505f0df1d1260cbf502790d093216966cc9ba77adbe38`,
UUID `F0DB7364-CF64-38A9-A9E5-3ADB16647A23`. This identity is recorded for
development reproducibility, not an instruction to install it.

All remote stages were exported and removed; the final core/aux stage occupied
11,540 KiB. Native diagnostics still report developer-owned staging files and
the SDK26.2 libkmod versus deployment15.0 warnings. No load test was performed.

## Remaining release gate

The measured active pipe uses TLB0; dormant powered requesters still need actual
retirement. Their address/ownership path must be implemented before release,
not bypassed or retested with another register-dump kext. Console virtual-address
translation alone does not acquire the existing framebuffer owner's lifetime.
Source/ROM research is ongoing using the existing evidence.

Detailed runtime recovery, firmware/GART/queue histories and final installed
core/aux/bundle hash binding remain packaging/evidence work. The runner has no
whole-process watchdog; a userspace hang can leave only partial progress and
before-capture evidence. Ordinary IORegistry properties cannot survive a kernel
panic. These limits are not reported as successful diagnostics or graphics.

Even a passing first offscreen test would establish only the exercised Metal
compute/render path. WindowServer, physical scanout handoff, HDMI/DP modes and
broader Metal support still require implementation/target validation. HDR is
deferred.
