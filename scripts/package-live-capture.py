#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Freeze a verified native live-capture kext and its exact build evidence.

Creates local artifacts only; never installs, loads, reboots or edits OpenCore.
Existing releases are immutable. Runtime success is never inferred from a build.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path, PurePosixPath
import plistlib
import tarfile
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def members(path):
    result = {}
    with tarfile.open(path, 'r:gz') as archive:
        for item in archive:
            name = PurePosixPath(item.name)
            if name.is_absolute() or '..' in name.parts or '\\' in item.name:
                raise ValueError('Unsafe archive name: ' + item.name)
            if item.isdir():
                continue
            if not item.isfile() or item.name in result or item.size > 32 * 1024 * 1024:
                raise ValueError('Unexpected archive member: ' + item.name)
            result[item.name] = archive.extractfile(item).read()
    return result


def render_handoff(package, binary, imports, power_probe=False, power_inventory=False,
                   current_state_config0=False, vcn_inventory=False, late_accelerator=False,
                   display_context=False):
    load_layout = '''
This package contains the full current early core driver. The IOAccelerator
service is now a separate, late-loading bundle so that OpenCore does not need
the SystemKC graphics-family superclass during BootKC injection. The previous
c39 package failed at precisely that boundary. This archive deliberately
contains only HackyNavi.kext for Kernel/Add; do not add the separately built
HackyNaviAccelerator.kext to OpenCore. The late service is not needed for this
capture test. Engine execution and late Metal discovery remain untested.
''' if late_accelerator else ''
    probe_instructions = '''
This follow-up preserves the raw display power failure omitted by the earlier
capture. BootDisplay now includes LastValue, LastReadSucceeded and LastPowerProbe
(Pipe, ReadsSucceededMask, StatusBefore, Config, StatusAfter). Mask bits 0/1/2
identify successfully read fields; zero without its validity bit is not a
hardware observation. Keep all values even when CaptureStatus still reports an
unsupported power state. This adds no register access or hardware activation.
''' if power_probe else ''
    if power_inventory:
        probe_instructions = '''
This follow-up handles the source-documented boot FORCEON condition only when
the actual reads show STATUS=0x10000000 and CONFIG=0x1, with stable confirmation.
It does not treat an unobserved CONFIG as zero or accept a conflicting gate
request. BootDisplay.PowerInventory independently records STATUS/CONFIG/STATUS
for all four pipes using at most12 reads of always-on power controls. It stops
on a failed read or all-ones response and never enters a HUBP window on that
failure. Each probe retains its validity mask; missing fields are not readings.
ClassifierAttempted distinguishes classification from power inventory only.
If both register passes finish and pass the stability checks, SnapshotReadComplete
is true and RawSnapshot preserves the first pass's register offsets/values even
if later plane decoding rejects the format. A pipe with WindowRead=false was
powered off; no HUBP samples are published for that pipe. These retained raw
values grant no hardware authority and cause no additional register reads.
Inventory success is not power ownership or engine readiness. No power register
is written, and there is no GPU activation or mode change in this test.
'''
        if current_state_config0:
            probe_instructions = probe_instructions.replace(
                'This follow-up handles the source-documented boot FORCEON condition only when\n'
                'the actual reads show STATUS=0x10000000 and CONFIG=0x1, with stable confirmation.\n',
                'This follow-up handles the actual target observation: current PGFSM state0\n'
                'with desired-state bit1, STATUS=0x10000000 and CONFIG=0x0 or0x1. Both\n'
                'configurations require stable confirmation and no explicit gate request.\n')
    if vcn_inventory:
        probe_instructions += '''
ColdAuxiliaryEngines.VcnPowerInventory records the always-readable POWER_STATUS
prerequisite before the full VCN checker. Up to four reads preserve two raw
samples per instance, with AttemptedMask, ReadSucceededMask and ValidMask.
Transport failure, all ones or changing values stop the inventory; a coherent
TILES_OFF indication allows the other POWER_STATUS pair but prevents the full
checker. ClassifierAttempted=false and NotCheckedWithoutVcnRegisterAccess mean
the stopped-engine check was not performed. They do not mean the engine is
retired or DMA is safe. No SMU power-up or register write was added.
'''
    if display_context:
        probe_instructions += '''
This build adds BootDisplayContext to investigate the remaining blocker from
the successfully loaded 9eda6a40f88c capture. It records 32 additional timing,
HUBP ancillary and VM-context registers in two bounded passes (at most208 MMIO
reads including power guards). It performs no new register writes. The earlier
BootDisplay classifier remains separate; its UnsupportedAddressMode result
is expected until the inherited address mode and unused pipes are understood.

Keep BootDisplayContext from device.plist/device.txt and LastCaptureObservation,
including CaptureStatus, NativeGenerationStillCurrent, RegisterOffsets, both
Passes, AttemptedMask, ReadSucceededMask, ValidMask, PowerValidMask, ChangedMask,
PowerChangedMask and LastPowerProbe. A zero without its ValidMask bit is not a
valid observation. Partial reads and changing live status are useful evidence;
they must not be replaced with zeros or summarized as disabled pipes.
CurrentHardwareAuthority, ReservationUseAllowed, EngineReadinessEstablished
and PhysicalAddressModeEstablished deliberately remain false. This collection
does not enable engines or authorize display-memory reuse. Keep the tested
9eda6a40f88c core/configuration as the rollback version.
'''
    return f'''# HackyNavi live-capture test: {package}

Prepare this specific experimental capture test on the user's existing OpenCore
USB. The driver-development session has prepared this package after earlier
diagnostic boot testing. Preserve the working USB and a rollback route. Do not load
on `victor@mini`, change the macOS installation, or reboot automatically.

This is a hardware-information test, not an accelerated driver release.
The unsigned x86_64 kext maps assigned BAR5, reads discovery, runtime ROM,
memory-layout and inherited-engine/display state. It performs bounded MMIO
selector writes to select discovery/ROM reads; it is not a purely read-only
PCI observer. It does not start engines, publish DMA, reset the GPU, program a
new display mode or create a Metal device. Current display output is inherited.
External firmware/SMM actors are not excluded by this capture service.

Executable SHA-256: `{binary['sha256']}`

Mach-O UUID: `{binary['uuid']}`

Native build resolved {imports} kernel imports on the build host. Native
compilation and symbol resolution do not establish target boot compatibility.
{load_layout}
Build-folder ownership diagnostics and deployment warnings are preserved in
the evidence. This exact build has not been loaded on the target at packaging.

1. Identify the actual USB by disk identity/capacity and existing `EFI/OC`.
   Back up its complete working EFI elsewhere and record the path. Preserve
   the last proven package and its configuration as the rollback version.
   Do not format a disk or modify an internal EFI partition.
2. Verify `SHA256SUMS.txt`. Replace the USB's whole
   `EFI/OC/Kexts/HackyNavi.kext` with this package's bundle. Check its executable
   hash after copying. Do not substitute the mutable development build folder.
3. Use `Kernel-Add-entry.plist` as one dictionary for `Kernel -> Add`, not as a
   replacement config. Keep exactly one enabled HackyNavi entry. Preserve
   other patches, kexts, quirks, SMBIOS and security settings.
4. Add exactly one `-hackynavi-device` to the effective boot arguments for this
   test. Preserve the existing arguments, including any current `-radvesa`.
   Leave `-hackynavi-engines` absent for this capture test.
   Do not spoof a device ID or inject a ROM. If NVRAM/config precedence prevents
   establishing the intended arguments, report the concrete condition; do not
   reset all NVRAM or broadly change security settings as a workaround.
5. Validate the plist and use `ocvalidate` from the installed OpenCore release.
   Save the USB identity, backup path, exact executable hash, argument change,
   configuration diff and validation results. Do not install to
   `/Library/Extensions` or rebuild system kernel collections. Let the user boot.
6. On the RX7900XTX target after boot, run:

   ```sh
   sh /path/to/{package}/collect-macos-diagnostics.sh
   ```

   Return the complete folder even if capture failed. Check command-status.txt
   for unavailable queries. If log/dmesg permissions failed, retain the failure
   and collect those queries with sudo separately without changing settings.

Expected evidence: the loaded UUID above, the existing passive `HackyNavi`
observer, and a `HackyNaviDevice` service. The latter should expose
`SnapshotStatus`, `CaptureStage`, and `CapturePowerGeneration`. A completed
capture has `SnapshotValid=true` and a matching nonzero `SnapshotGeneration`
and `PowerGeneration`. Nested ROM/posted/auxiliary/display statuses may still
report unsupported/unavailable state: preserve them verbatim. Completion of
capture is not readiness for engines. `LastCaptureObservation` is historical,
has no current hardware authority, and survives discarding live snapshots.
`AccelerationAvailable` and `DisplayInitializationAvailable` remain false.
{probe_instructions}

Also return whether the desktop stayed visible, connector used, any hang/panic,
and whether sleep/reboot occurred before collection. Do not initiate sleep,
stress tests or additional hardware activation for this capture test.

Rollback: restore the saved working config and kext. If restoring the passive
build, remove this test's `-hackynavi-device` argument from its actual source. Alternatively disable
the HackyNavi entry and remove the argument using the existing recovery route.
If boot fails, retain loader/panic evidence before making further changes.
Do not report GPU execution, controlled scanout or Metal from this test.
'''


def package(native, output):
    report_path = native / 'build-report.json'
    report = json.loads(report_path.read_text(encoding='utf-8'))
    if report.get('build_exit_code') != 0 or report.get('kernel_libraries_exit_code') != 0 or \
            report.get('kernel_symbols_unresolved') != [] or not report.get('remote_stage_removed'):
        raise ValueError('Require a completed native build with resolved imports and removed staging')
    spec = importlib.util.spec_from_file_location('inspect_kext', ROOT / 'scripts/inspect-kext.py')
    inspector = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(inspector)
    binary = inspector.inspect(native / 'HackyNavi.kext')
    boot_imports = inspector.boot_injection_imports(binary, report.get('kernel_symbol_providers', {}))
    if not boot_imports['all_imports_in_boot_collection']:
        raise ValueError('Early OpenCore package has imports not proven in BootKC: ' +
                         ', '.join(boot_imports['imports_not_proven_in_boot_collection']))
    built = report['native_bundle']
    if not binary['uuid'] or any(binary[k] != built[k] for k in ('sha256', 'uuid', 'bytes', 'bundle_identifier')):
        raise ValueError('Native report does not identify this executable')
    source_archive = (native / 'source.tar.gz').read_bytes()
    artifact_archive = (native / 'artifact.tar.gz').read_bytes()
    if sha(source_archive) != report['archive_sha256'] or sha(artifact_archive) != report['artifact_sha256']:
        raise ValueError('Archive hash differs from native report')
    sources = members(native / 'source.tar.gz')
    expected = {entry['path']: entry for entry in report['files']}
    if len(expected) != len(report['files']) or sources.keys() != expected.keys():
        raise ValueError('Source manifest members differ')
    for name, data in sources.items():
        if sha(data) != expected[name]['sha256'] or len(data) != expected[name]['bytes']:
            raise ValueError('Source snapshot differs: ' + name)
    device = sources['kext/HackyNavi/HackyNaviDevice.cpp']
    if b'LastCaptureObservation' not in device or b'-hackynavi-device' not in device:
        raise ValueError('Selected snapshot predates stage-preserving live capture')
    payload = members(native / 'artifact.tar.gz')
    actual = {p.relative_to(native).as_posix(): p.read_bytes()
              for p in (native / 'HackyNavi.kext').rglob('*') if p.is_file()}
    if payload != actual or any(not n.startswith('HackyNavi.kext/') for n in payload):
        raise ValueError('Native bundle differs from its archived build artifact')
    package_id = 'HackyNavi-live-capture-' + binary['sha256'][:12]
    destination = output / package_id
    zipped = output / (package_id + '.zip')
    if destination.exists() or zipped.exists():
        raise FileExistsError('Refusing to overwrite immutable release ' + package_id)
    entry = {'Arch': 'x86_64', 'BundlePath': 'HackyNavi.kext', 'Enabled': True,
             'ExecutablePath': 'Contents/MacOS/HackyNavi', 'PlistPath': 'Contents/Info.plist',
             'MinKernel': '24.0.0', 'MaxKernel': '24.99.99',
             'Comment': package_id + '; capture only, no acceleration'}
    payload['Kernel-Add-entry.plist'] = plistlib.dumps(entry)
    power_probe = b'"LastPowerProbe"' in device and b'"LastReadSucceeded"' in device
    power_inventory = b'"PowerInventory"' in device and b'"ClassifierAttempted"' in device
    display_source = b''.join(sources['src/boot_display.cpp'].split())
    # Both reviewed implementations accept the observed CONFIG0/current-ON
    # pair. Later source factored current-state decoding into named helpers.
    current_state_config0 = b'if(power==0x90000000?config!=0x100:config>1)' in display_source or all(
        text in display_source for text in (
            b'boolcurrentOn(uint32_tstatus){returnstatus==0||status==0x10000000;}',
            b'boolcurrentOff(uint32_tstatus){returnstatus==0x80000000||status==0x90000000;}',
            b'if(currentOff(power)?config!=0x100:config>1)'))
    vcn_inventory = b'"VcnPowerInventory"' in device and b'inventoryVcnPower' in sources['src/cold_aux.cpp']
    late_accelerator = 'kext/HackyNaviAccelerator/Info.plist' in sources and \
        'com.apple.iokit.IOGraphicsFamily' not in binary['dependencies']
    display_context = b'"BootDisplayContext"' in device and all(name in sources for name in (
        'include/hackynavi/boot_display_context.hpp', 'src/boot_display_context.cpp'))
    payload['HANDOFF.md'] = render_handoff(package_id, binary, len(binary['undefined_kernel_symbols']),
        power_probe, power_inventory, current_state_config0, vcn_inventory, late_accelerator, display_context).encode()
    payload['collect-macos-diagnostics.sh'] = (ROOT / 'scripts/collect-macos-diagnostics.sh').read_bytes().replace(b'\r\n', b'\n')
    payload['evidence/source.tar.gz'] = source_archive
    payload['evidence/build-report.json'] = report_path.read_bytes()
    payload['evidence/build.log'] = (native / 'build.log').read_bytes()
    payload['evidence/macho-report.json'] = (json.dumps(binary, indent=2) + '\n').encode()
    metadata = {'schema_version': 1, 'package_id': package_id, 'scope': 'opt-in live capture only',
                'boot_argument': '-hackynavi-device', 'executable_sha256': binary['sha256'],
                'macho_uuid': binary['uuid'], 'load_tested': False, 'gpu_execution_tested': False,
                'boot_collection_import_check': boot_imports,
                'late_accelerator_separate': late_accelerator,
                'display_control_tested': False, 'metal_available': False,
                'display_current_state_config0': current_state_config0,
                'vcn_power_prerequisite': vcn_inventory,
                'boot_display_context_inventory': display_context,
                'native_source_archive_sha256': report['archive_sha256'],
                'native_build_report_sha256': sha(payload['evidence/build-report.json'])}
    payload['PACKAGE.json'] = (json.dumps(metadata, indent=2) + '\n').encode()
    payload['SHA256SUMS.txt'] = ''.join(f'{sha(data)}  {name}\n' for name, data in sorted(payload.items())).encode()
    output.mkdir(parents=True, exist_ok=True)
    staging = output / ('.packaging-' + uuid.uuid4().hex)
    staging.mkdir()
    for name, data in payload.items():
        target = staging / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    temporary_zip = output / (staging.name + '.zip')
    with zipfile.ZipFile(temporary_zip, 'x', zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(payload.items()):
            archive.writestr(package_id + '/' + name, data)
    with zipfile.ZipFile(temporary_zip) as archive:
        if archive.testzip() or {n: archive.read(n) for n in archive.namelist()} != \
                {package_id + '/' + n: data for n, data in payload.items()}:
            raise ValueError('Frozen ZIP verification failed; staging retained')
    staging.rename(destination)
    temporary_zip.rename(zipped)
    return {'package': str(destination), 'zip': str(zipped), 'zip_bytes': zipped.stat().st_size,
            'zip_sha256': sha(zipped.read_bytes()), 'files_verified': len(payload), **metadata}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/releases')
    args = parser.parse_args()
    print(json.dumps(package(args.native_dir.resolve(), args.output.resolve()), indent=2))
