#!/bin/sh
# SPDX-License-Identifier: MIT
# Collect evidence only. Does not load/unload a kext or change system settings.
set -u

if [ "$(/usr/bin/uname -s)" != Darwin ]; then
    printf '%s\n' 'Run this collector on the target macOS installation.' >&2
    exit 1
fi
if [ "$#" -gt 1 ]; then
    printf '%s\n' 'Usage: sh collect-macos-diagnostics.sh [new-output-directory]' >&2
    exit 1
fi
umask 077
output=${1:-"$HOME/Desktop/HackyNavi-diagnostics-$(/bin/date +%Y%m%d-%H%M%S)"}
# Never reuse a directory: evidence from separate boots must not be mixed.
/bin/mkdir "$output" || exit 1
output=$(cd "$output" && /bin/pwd) || exit 1
capture_attempted=0
capture_failed=0

capture() {
    name=$1
    shift
    "$@" > "$output/$name" 2>&1
    result=$?
    printf '%s exit=%s\n' "$name" "$result" >> "$output/command-status.txt"
    capture_attempted=$((capture_attempted + 1))
    if [ "$result" -ne 0 ]; then capture_failed=$((capture_failed + 1)); fi
}

capture macos.txt /usr/bin/sw_vers
capture kernel.txt /usr/bin/uname -a
capture boot-time.txt /usr/sbin/sysctl kern.boottime
capture boot-args.txt /usr/sbin/sysctl kern.bootargs
capture kexts.txt /usr/bin/kmutil showloaded --list-only
capture observer.plist /usr/sbin/ioreg -a -r -c HackyNavi -l
capture observer.txt /usr/sbin/ioreg -r -c HackyNavi -l -w 0
capture device.plist /usr/sbin/ioreg -a -r -c HackyNaviDevice -l
capture device.txt /usr/sbin/ioreg -r -c HackyNaviDevice -l -w 0
# Keep accelerator discovery, inherited framebuffer properties and actual
# display EDID in the same boot's evidence. These queries do not open a Metal
# device, submit work or treat registry properties as hardware authority.
capture accelerator.plist /usr/sbin/ioreg -a -r -c HackyNaviAccelerator -l
capture accelerator.txt /usr/sbin/ioreg -r -c HackyNaviAccelerator -l -w 0
capture framebuffers.plist /usr/sbin/ioreg -a -r -c IOFramebuffer -l
capture displays.plist /usr/sbin/ioreg -a -r -c IODisplayConnect -l
capture target-cpu.txt /usr/sbin/sysctl machdep.cpu.brand_string
capture sip-status.txt /usr/bin/csrutil status
# Topology without -l avoids dumping unrelated device properties. This helps
# identify GPU/audio providers and their attached clients on later live tests.
capture service-tree.txt /usr/sbin/ioreg -p IOService -w 0
# IOLog output from early startup may exist only in the kernel message buffer.
# A permission failure is recorded; never elevate or change settings silently.
capture kernel-message-buffer.txt /sbin/dmesg
capture hackynavi-log.txt /usr/bin/log show --last boot --style compact --info --debug \
    --predicate 'process == "kernel" AND eventMessage CONTAINS[c] "HackyNavi"'
# Include userspace bundle/runtime failures as well as the kernel stage log.
# Deliberately avoid querying every application's Metal log or all NVRAM.
capture graphics-log.txt /usr/bin/log show --last boot --style compact --info --debug \
    --predicate 'eventMessage CONTAINS[c] "HackyNavi" OR process BEGINSWITH "hackynavi-metal-"'

capture_complete=true
if [ "$capture_failed" -ne 0 ]; then capture_complete=false; fi
printf 'commands_attempted=%s\ncommands_succeeded=%s\ncommands_failed=%s\nall_commands_succeeded=%s\n' \
    "$capture_attempted" "$((capture_attempted - capture_failed))" "$capture_failed" "$capture_complete" \
    > "$output/collection-summary.txt" || exit 1

cat > "$output/README.txt" <<'EOF'
This folder contains observations from one macOS boot. It is not a pass report.
Check command-status.txt: a failed query is missing evidence, not a driver failure.
collection-summary.txt separately reports whether every collection command succeeded.
Collector exit 0 means collection finished; it does not imply every query succeeded
or that a successful but empty query found a device. Missing optional queries do
not prevent the graphics runner from attempting work on a verified live engine.
If kmutil/log/dmesg report insufficient permissions, rerun those queries with sudo and
save the additional output here; do not change kext/security settings to collect.

Expected for the first, passive test:
- org.hackynavi.driver.HackyNavi in kexts.txt
- a HackyNavi IORegistry service with DiagnosticMode=PassivePCISnapshot
- ObservedDeviceCount >= 1 and a 1002:744c snapshot on the target 7900 XTX
- AccelerationImplemented=false (expected; this build does not enable Metal)
- no HackyNaviDevice service, because -hackynavi-device is intentionally absent
VBIOS Status=NotProvidedByOS is a valid observation, not a boot-test failure.

For a separately identified live-capture test package with -hackynavi-device:
- the passive observer should still be present
- HackyNaviDevice should report SnapshotStatus and CaptureStage
- successful capture has SnapshotValid=true and matching nonzero power/snapshot generations
- LastCaptureObservation is historical evidence only, even after power-off
- return failed/revoked capture and partial observations too; do not hide errors
- AccelerationAvailable and DisplayInitializationAvailable remain false
Only enable the live-capture argument when the selected package's handoff
explicitly calls for it. The collector itself never changes boot arguments.

For an explicitly issued graphics-test package:
- preserve PreEngineCapture/LastCaptureObservation, LastEngineObservation and
  LastRetirementObservation, including incomplete or rejected fields
- EngineStartupVerified reports kernel startup only; an accelerator registry
  entry is not evidence that public Metal discovery or a shader succeeded
- return each graphics tool's original stdout/stderr and exit status alongside
  this directory; only actual compute/pixel readback can establish that result
- an empty accelerator/framebuffer query or a failed query remains evidence;
  do not hide it or repeat the boot merely to fill that field
The collector does not run the graphics tools or alter SIP. The graphics test's
runner must invoke the exact packaged tools after its matching startup checks.

If the observer exists but no matching GPU appears, return that evidence too.
service-tree.txt records provider/client topology without property dumps.
kernel-message-buffer.txt can contain early IOLog lines absent from unified log.
Record visible desktop behavior, actual boot USB, any hang/panic, and whether
the GPU identity is being spoofed. Boot arguments may contain private values;
review these files before sharing outside this development session.
EOF
printf 'Diagnostics saved to: %s\n' "$output"
printf '%s\n' 'Return this folder and the tested package ID; no load/acceleration success is inferred.'
