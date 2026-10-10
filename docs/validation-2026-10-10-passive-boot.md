# First target boot: passive diagnostic evidence

The supplied `E:/HackyNaviLogs/20261010-225812-709` collection demonstrates
that the frozen diagnostic kext loaded and observed the user's physical GPU.
This is the first target runtime evidence. It establishes the passive test
stage only; it does not establish GPU execution, modesetting or acceleration.

| Observation | Evidence |
| --- | --- |
| OS | macOS 15.8.1, build 24H32; Darwin 24.6.0, x86_64 |
| CPU | AMD Ryzen 9 7900X, reported by the supplied target-cpu record |
| Loaded extension | `org.hackynavi.driver.HackyNavi`, version 0.1.0, present in successful `kmutil showloaded` output |
| Loaded UUID | `EA072A1A-330B-39A5-AA3B-421416E9EFF2`; matches LC_UUID in the separately selected frozen binary |
| Binary SHA-256 | `dd4f86a48836ca83667140fadd846e4a6b98f62eefd6be00d199a874991f6d47` |
| Observer | `HackyNavi`, registered/matched/active; `PassivePCISnapshot`, observed count 1 |
| GPU identity | `1002:744c`, revision `c8`, subsystem `148c:2422`, display class `030000` |
| Experimental service | Absent; no `-hackynavi-device` in effective boot arguments |
| Acceleration | Observer explicitly reports false |
| VBIOS property | `NotProvidedByOS`; expected possible outcome, not a failed PCI observation |
| Collector | All ten command statuses zero; filtered kernel log contains only its header |

The captured PCI BARs are:

| BAR | Kind | Configured address |
| --- | --- | --- |
| 0 | Prefetchable 64-bit memory | `0x880000000` |
| 2 | Prefetchable 64-bit memory | `0x890000000` |
| 4 | I/O | `0x1000` |
| 5 | Non-prefetchable 32-bit memory | `0xdfa00000` |

These addresses agree with the raw configuration dwords. The XML serializer
represents high-bit 32-bit OSNumber values as negative integers; reinterpret
their declared 32-bit width before decoding. The large values printed in
the text dump do not imply oversized BARs. **BAR sizes were not probed.**

The log contains `-radvesa` and loaded WhateverGreen/Lilu. Keep the known boot
configuration with the installation session; this passive test did not change
or test their acceleration behavior. The preparation receipt describes other
installation-session changes; it is provenance data, not driver-runtime proof.
The receipt's and package's historical `load_tested=false` fields are retained;
the new runtime evidence supersedes that historical state for this binary's
passive mode. The frozen package itself must not be rewritten.

## Reproduce the evidence checks

From the Windows repository root:

```powershell
python scripts/analyze-macos-diagnostics.py E:/HackyNaviLogs/20261010-225812-709 --bundle build/releases/HackyNavi-diagnostic-dd4f86a48836/HackyNavi.kext --output build/target-evidence/20261010-225812-709/analysis.json
```

The analyzer reads the successful collector results, associates the loaded
UUID with the selected executable, validates identity and BAR consistency,
and hashes the original evidence. The positive real capture passes. Modified
copies with a different UUID, failed collection, active experimental argument,
changed BAR address, or an unexpected device service are rejected. The originals
are not modified. A matching UUID is not a hash of runtime kernel memory.

Key raw evidence hashes:

| File | SHA-256 |
| --- | --- |
| `observer.plist` | `bd82d0a9465e1c3dbf5d729be83a434e94afd505a7f38b9ad1bb349c4d1d753b` |
| `kexts.txt` | `410e30a15ddb69f60a3c3a7fd2742738979cb82e50c899ceee6aac842f8992ec` |
| `command-status.txt` | `92d2a408807545312a9be71af3350f92de9d577866dcd2e3c0a7f3d6fa38756a` |

## What changes next

Loading and PCI detection are now demonstrated for the frozen observer build.
The next target capture should exercise the reviewed opt-in `HackyNaviDevice`
path: actual BAR5 mapping, live discovery, memory layout, runtime ROM and
inherited display/engine state. The sleepable-worker cancellation fix and
65-source native compile/link checks have now passed; that newer binary is
distinct from this boot-tested one. Finish capture-path review and a separate
traceable test handoff before enabling it. Do not activate the engine
coordinator from these PCI-only results.

The evidence does not contain a shader result, GPU fence, live IP discovery,
MMIO/VRAM mapping, verified reset, audio sibling ownership, visible-mode change,
HDMI/DP training, hotplug, sleep/wake recovery, or a Metal device. An empty
filtered kernel log is not proof that no system faults occurred. Continue the
engine lifecycle, real AIR compilation, isolated submission, and display work.
