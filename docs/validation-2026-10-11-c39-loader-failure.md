# Confirmed OpenCore load failure for c39afcee43f9

The target booted macOS 15.8.1 / 24H32, but OpenCore rejected HackyNavi before
its startup code could run. This is a driver packaging/dependency defect, not
GPU engine or display-test evidence.

The selected executable is
`c39afcee43f96070f8a230f11ff890360cc6edbe2bc2bb7ab67ef743b9560180`,
UUID `100496F0-532E-374A-B4A4-3D6E0D61E3D0`, issued in
`build/releases/current-20261011/HackyNavi-live-capture-c39afcee43f9.zip`.
The preparation receipt identifies intended media; it cannot establish a
successful load. No matching UUID appears in the target's loaded-kext list.

## Actual target evidence

`E:/HackyNaviLogs/live-c39afcee43f9-20261011-144146-771` contains successful
collector commands, the effective `-hackynavi-device` argument, no engines
argument, an absent HackyNavi loaded-kext entry, empty observer/device queries,
and no HackyNavi service in the full IORegistry topology. Empty ioreg output
with exit zero means no match; the analyzer now preserves that negative result.
The same analyzer still passes the previously successful dbbcb5ec capture.

At the user's request the USB EFI logs were copied with an elevated Windows
read. The script verified the recorded hp x900w USB identity and EFI partition
GUID, copied the three newest OpenCore logs into this workspace, and did not
edit the USB. The decisive log is
`build/target-evidence/live-c39afcee43f9-20261011-144146-771/opencore/opencore-2026-10-11-143848.txt`,
SHA-256 `9d043799e2069ba0668c8206c10fb2fa4c89c6411c5723b753ec7bcf31222c3c`.
Its lines 688–690 report:

```text
OCAK: com.apple.iokit.IOGraphicsFamily not in Boot KC, using kernel stub for symbol-only deps
OCAK: Vtable patching failed for kext org.hackynavi.driver.HackyNavi
OC: Prelinked injection HackyNavi.kext (HackyNavi-live-capture-c39afcee43f9; capture only, no acceleration) - Invalid Parameter
```

The source analysis is in `build/loader-c39-audit-1/report.json`; the original
report predates access to this log and correctly labels its conclusion as a
candidate. The target log now confirms that candidate. Native library lookup
had resolved all 414 imports, but four IOAccelerator imports came exclusively
from SystemKernelExtensions.kc. OpenCore's BootKC symbol-only fallback cannot
provide that superclass vtable. The earlier proven binary did not import it.

## Implemented correction and replacement test

The early core kext is now separate from a late, genuine IOAccelerator subclass. The
early core retains engines, native sessions, memory ownership, submission and
recovery. The late accelerator retains the real graphics-family inheritance
and Metal discovery relationship, with publication admitted by the core's
actual initialized owner and retirement gate. It must not be included in the
early OpenCore injection list.

The build/package tooling now distinguishes symbol resolution across all
kernel collections from availability in BootKC. Early packages must prove
every actual import has a BootKC provider in the build-host evidence. That
check does not prove target loadability; the corrected bundle still needs a
real target boot. The old issued archive remains immutable failure evidence.

The replacement is built in `build/macos-accelerator-split-1`: 110 core units,
two auxiliary units, 412 BootKC core imports and 289 resolved auxiliary imports.
Independent review binds all 279 source inputs, the two separate bundle
archives and ten actual embedded firmware images. Its report is
`build/boot-collection-gate-review-2/native-artifact-review.json`, SHA-256
`dc1a7df9ec55933f1bdb8bcd2ac1059f56ed59ae262a1a1b74da451d0e7fbb8c`.

Replacement package:
`build/releases/loader-fix-20261011/HackyNavi-live-capture-9eda6a40f88c.zip`,
SHA-256 `771fe568a15833f21863b821d3fd29500e547b3dad0d8ccd4bac0a84eeadbe92`.
Its current source includes the PSP TA ownership implementation, with no
automatic TA startup activation. The separate late accelerator is not included
for early injection. The next target boot checks the corrected load and capture
path with engines absent; it must precede an engine-enabled test.

No GPU execution, Metal rendering, or display takeover occurred in this test.
