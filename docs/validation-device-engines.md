# Device engine integration validation

This records implementation, static review, host tests and native linking.
It does not record a target boot, GPU execution, recovery or controlled display.
The delivered target test remains `HackyNavi-live-capture-1e4040229efa`.

## Implemented path

The additional `-hackynavi-engines` opt-in selects a real sleepable activation
worker in `HackyNaviDeviceEngines.cpp`; `-hackynavi-device` remains necessary
for Device matching. The worker owns publication of the GPU/audio function
group. Notifications only record a wake, including callbacks delivered before
the group factory returns. Both primary power-on and paired notifications
increment the serial used to requeue a wake received while the worker is pending.

A fresh actual Operation precedes mapping and Lifecycle construction. Device
publishes the Lifecycle before registering its retirement handler, and registers
the handler before recovery preparation or engine control writes. Startup is
one-shot. The Engine performs its existing real fence/shader/DMCUB checks; this
code has not executed on the target. No Metal or display capability is advertised.

Primary PM-off, termination, shutdown and close join paired retirement outside
the Device mutex before ACK, BAR unmap or PCI close. Primary retirement revokes
before checking handler registration and drains/rechecks the unarmed race.
Failed activation destroys Operation before the same registering worker requests
OwnerFailure. Only a nonempty subset of PowerDown and OwnerFailure can enter PCI
recovery. Terminal reasons atomically prohibit recovery; overlap after entry
uses the explicit fail-stop policy. Actual retirement failure or deadline expiry
panics while retaining potentially live owners.

GART Access guards bind the exact Operation or RetirementScope through firmware,
graphics/SDMA, VM and display callbacks. Reset Access binds the exact controller,
provider, owner, mapping, thread and serial. Guards retain their controller and
clear borrowed scope bindings before releasing it. One Reset guard spans normal
stop, reset, ATOM initialization, hardware proof verification, typed owner release,
proof transfer and permanent-shadow refresh. The MODE1 blackout uses the group's
CPU-only recovery predicate rather than PCI/D0 probing. Unpublished CPU cleanup
does not require a readable removed device.

The display owner also captures seven firmware-reported DAL DPM tables twice,
with a monotonic total deadline and private generation/serial provenance. This
is not measured frequency, a correlated operating-state model, a board DML
bounding box, a watermark policy, or modeset authorization.

## Review and checks

- Device review found one missed-wake race: primary power-on could arrive while
  a worker was pending, before a paired group existed. The primary PM-on serial
  increment fixes that concrete path; the reviewer checked the final source.
- Separate reviews checked Group OwnerFailure/primary retirement and descendant
  guard lifetime/cleanup call paths. No further concrete defect was identified.
  Static review does not establish runtime ordering or memory safety on hardware.
- New/existing display DAL suites passed 489,675 and 5,995 host sanitizer checks.
  Fresh CMake integration built both targets and passed 2/2 in 2.14 seconds.
  CMake now configures 62 tests; this was not a full 62-test run.
- Focused real Kernel SDK compilation passed for Device/Group, GART owners,
  Engine, Lifecycle and Reset/ATOM. The final native link covers the later
  missed-wake fix and the complete current header/source snapshot.

## Exact native build

Report: `build/macos-device-engines-final/build-report.json`.

| Item | Observed result |
| --- | --- |
| Build host | Intel macOS 15.8 / 24H22, Apple Clang 17, SDK 26.2 |
| Intended runtime | x86_64 macOS 15.8.1; deployment baseline 15.0 |
| Translation units | 81, including generated pinned firmware |
| Kernel imports | 364 resolved; zero unresolved |
| Source snapshot | 3,810,207 bytes, all recorded hashes independently checked against the working tree |
| Source archive SHA-256 | `1cef855e309eb4eff6f24824dd4849b78e6c38ca4f2003c0d1fd4ddac6b55bae` |
| Executable | 2,904,680 bytes |
| Executable SHA-256 | `c3bcacee8f9285e73f592908437e1d0dbcdef299cf32626d1268090f3b0d4b0a` |
| Mach-O UUID | `70DE9C99-D805-3649-AE60-263FAE5C87C1` |
| Report SHA-256 | `d8b4bebda8370dd392968919c435ec35990dd4cced36c7c63cef8ecd6cf9b22b` |
| Remote staging | 8,108 KiB; unique stage removed |
| Installation/load | None on build host or target |

The native build log retains build-folder ownership errors (501:20 versus root)
from the no-load diagnostic and SDK libkmod deployment-version warnings.
Dependency/import resolution passed; it is not a successful target load check.

The preserved `build/macos-device-engines` first link precedes the reviewed
primary wake fix. Its Device source hash intentionally differs from the working
tree; use the `-final` report for the complete implementation above.

## Target handoff

All 14 manifest-listed files in the unchanged `1e4040229efa` package were checked
against both the extracted directory and ZIP. ZIP SHA-256 remains
`1cf20912503571e173c8b9a7044ba13afa3340a49641c4779dd1cbb5a2734078`.
Use its own HANDOFF.md and collector to obtain the missing raw DOMAIN0 value.
Do not attribute the new engine code to that diagnostic executable. The user
reported HDMI and no incident during the preceding `6a3a8173c6e2` capture.

Before an engine test can establish progress, it must demonstrate real paired
ownership, the activation stage/result, GPU-written fence/shader output and
retirement behavior. Native framebuffer/IOSurface, user submission integration,
Metal, complete HDMI/DP modesets and high-refresh FRL/DSC remain unfinished.
