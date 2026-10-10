#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Analyze passive or explicitly selected live-capture boot evidence.

Does not execute files, trust preparation claims as runtime evidence, or mutate
the evidence directory. Pass --bundle explicitly; it is never selected by logs.
An exit status of zero means only the selected diagnostic checks passed.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import re

IDENTIFIER = "org.hackynavi.driver.HackyNavi"
REQUIRED = ("macos.txt", "kernel.txt", "boot-args.txt", "kexts.txt",
            "observer.plist", "device.plist", "command-status.txt")
COMMANDS = ("macos.txt", "kernel.txt", "boot-args.txt", "kexts.txt",
            "observer.plist", "device.plist")


def live_capture(items, errors):
    if not isinstance(items, list):
        raise ValueError('Device plist must contain an ioreg result array')
    matches = [p for p in items if isinstance(p, dict) and
               p.get('IOObjectClass', p.get('IOClass')) == 'HackyNaviDevice']
    if len(matches) != 1:
        errors.append('Exactly one HackyNaviDevice capture service was not found')
        return None
    p = matches[0]
    if p.get('Mode') != 'ExperimentalDeviceBringup' or \
            p.get('AccelerationAvailable') is not False or p.get('DisplayInitializationAvailable') is not False:
        errors.append('Unexpected capture-service mode/capability report')
    identity = p.get('PCIIdentity')
    if type(identity) is not int or identity & 0xffffffff != 0x744c1002 or p.get('PCIRevision') != 0xc8:
        errors.append('Capture service PCI identity/revision differs from target')
    generation = p.get('PowerGeneration')
    if type(generation) is not int or generation <= 0 or \
            p.get('SnapshotGeneration') != generation or p.get('CapturePowerGeneration') != generation:
        errors.append('Capture generations are absent, stale, or inconsistent')
    if p.get('SnapshotValid') is not True or p.get('SnapshotStatus') != 'ValidatedPowerUpCapture' or \
            p.get('CaptureStage') != 'Complete':
        errors.append('Current power-up capture is not complete and valid')
    register_bytes = p.get('RegisterBytes')
    if p.get('RegisterBAR') != 5 or type(register_bytes) is not int or not 0x58188 <= register_bytes <= 16 * 1024 * 1024:
        errors.append('Capture has no bounded BAR5 mapping')
    discovery = p.get('Discovery', {})
    memory = p.get('MemoryLayout', {})
    history = p.get('LastCaptureObservation', {})
    if not all(isinstance(v, dict) for v in (discovery, memory, history)):
        raise ValueError('Capture property must be a dictionary')
    if discovery.get('Status') not in ('Validated', 'ValidatedInventoryTruncated') or \
            discovery.get('AcquisitionStatus') != 'ok' or discovery.get('RegisterLayoutStatus') != 'ok':
        errors.append('Validated discovery and register layout are missing')
    if memory.get('Status') != 'ValidatedRegisterSnapshot' or memory.get('AllocationStatus') != 'ReservationsNotEstablished':
        errors.append('Expected diagnostic memory-layout observation is missing')
    if history.get('HistoricalEvidenceOnly') is not True or history.get('CurrentHardwareAuthority') is not False:
        errors.append('Historical capture record lacks its explicit authority boundary')
    # Nested firmware/scanout outcomes may be unsupported even after successful
    # diagnostic capture. Preserve them; they are not engine readiness claims.
    keys = ('SnapshotStatus', 'CaptureStage', 'CapturePowerGeneration', 'PowerGeneration',
            'SnapshotGeneration', 'SnapshotValid', 'RegisterBytes', 'DiscoveryAcquisition',
            'Discovery', 'RuntimeROM', 'PostedColdState', 'ColdAuxiliaryEngines',
            'MemoryLayout', 'BootDisplay', 'LastCaptureObservation')
    return {key: p[key] for key in keys if key in p}


def analyze(folder, bundle, mode='passive'):
    if mode not in ('passive', 'live-capture'):
        raise ValueError('Unknown diagnostic mode')
    folder = folder.resolve(strict=True)
    spec = importlib.util.spec_from_file_location(
        "hackynavi_inspect", Path(__file__).with_name("inspect-kext.py"))
    inspector = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(inspector)
    binary = inspector.inspect(bundle)
    if binary["bundle_identifier"] != IDENTIFIER:
        raise ValueError("Selected bundle has an unexpected identifier")
    evidence, hashes = {}, {}
    for name in REQUIRED:
        path = folder / name
        if path.is_symlink() or path.resolve().parent != folder:
            raise ValueError("Evidence must be an immediate regular file: " + name)
        if not path.is_file() or path.stat().st_size > 16 * 1024 * 1024:
            raise ValueError("Missing or oversized evidence: " + name)
        data = path.read_bytes()
        evidence[name] = data
        hashes[name] = hashlib.sha256(data).hexdigest()

    errors, notes = [], []
    text = lambda name: evidence[name].decode("utf-8-sig")
    statuses = {}
    for line in text("command-status.txt").splitlines():
        match = re.fullmatch(r"([A-Za-z0-9_.-]+) exit=(\d+)", line.strip())
        if not match or match[1] in statuses:
            raise ValueError("Malformed or duplicate command status")
        statuses[match[1]] = int(match[2])
    for command in COMMANDS:
        if statuses.get(command) != 0:
            errors.append("Missing/failed evidence command: " + command)

    loaded = re.findall(re.escape(IDENTIFIER) +
                        r"\s+\(([^)]+)\)\s+([0-9A-Fa-f-]{36})(?=\s|$)",
                        text("kexts.txt"))
    uuid_match = len(loaded) == 1 and binary["uuid"] is not None and \
        loaded[0][1].upper() == binary["uuid"]
    if not uuid_match:
        errors.append("Loaded kext UUID absent, ambiguous, or different from selected binary")
    bootargs = text("boot-args.txt")
    experimental = re.search(r"(?:^|\s)-hackynavi-device(?:=|\s|$)", bootargs) is not None
    if experimental and mode == 'passive':
        errors.append("Experimental device argument is active; this is not a passive-only test")
    if not experimental and mode == 'live-capture':
        errors.append('Live capture requires the effective experimental device argument')
    kernel = text("kernel.txt")
    version = dict(re.findall(r"^(ProductVersion|BuildVersion):\s*(\S+)\s*$",
                              text("macos.txt"), re.MULTILINE))
    target_os = version.get("ProductVersion") == "15.8.1" and \
        re.search(r"\b24\.6\.0\b", kernel) is not None and "x86_64" in kernel
    if not target_os:
        errors.append("Collected OS/kernel does not match the intended macOS 15.8.1 x86_64 target")

    # ioreg succeeds with empty output when no matching service exists. Keep
    # that negative observation in the report instead of aborting plist parsing
    # and losing the independently collected loaded-kext result.
    observer_data = evidence["observer.plist"].strip()
    observers = plistlib.loads(observer_data) if observer_data else []
    if not isinstance(observers, list):
        raise ValueError("Observer plist must contain the ioreg result array")
    matches = [item for item in observers if isinstance(item, dict) and
               item.get("CFBundleIdentifier") == IDENTIFIER and
               item.get("IOObjectClass", item.get("IOClass")) == "HackyNavi"]
    devices = []
    if len(matches) != 1:
        errors.append("Exactly one HackyNavi observer was not found")
    else:
        observer = matches[0]
        if observer.get("DiagnosticMode") != "PassivePCISnapshot" or \
                observer.get("AccelerationImplemented") is not False:
            errors.append("Observer does not report the expected passive mode")
        snapshots = observer.get("DeviceSnapshots", {})
        if not isinstance(snapshots, dict):
            raise ValueError("Invalid DeviceSnapshots dictionary")
        if observer.get("ObservedDeviceCount") != len(snapshots) or not snapshots:
            errors.append("No observed device or inconsistent snapshot count")
        for key, snapshot in snapshots.items():
            if not isinstance(snapshot, dict):
                raise ValueError("Invalid device snapshot")
            identity_ok = (snapshot.get("VendorID"), snapshot.get("DeviceID"),
                           snapshot.get("PCIIdentityRaw")) == (0x1002, 0x744c, 0x744c1002)
            if not identity_ok or snapshot.get("Status") != "CapturedBestEffort":
                errors.append("PCI snapshot missing matching readable identity: " + key)
            board_ok = (snapshot.get("RevisionID"), snapshot.get("SubsystemVendorID"),
                        snapshot.get("SubsystemDeviceID")) == (0xc8, 0x148c, 0x2422)
            if not board_ok:
                errors.append("PCI revision/subsystem differs from the user's target board")
            bars, indices = [], set()
            for bar in snapshot.get("ConfiguredBARs", []):
                index, kind = bar.get("Index"), bar.get("Kind")
                if type(index) is not int or index not in range(6) or index in indices:
                    raise ValueError("Invalid/duplicate PCI BAR index")
                indices.add(index)
                raw = bar.get("RawLow")
                if type(raw) is not int or not -(1 << 31) <= raw <= 0xffffffff:
                    raise ValueError("Invalid PCI configuration dword")
                # ioreg XML renders 32-bit OSNumber values as signed integers.
                raw &= 0xffffffff
                address = bar.get("ConfiguredAddress")
                expected = None
                if kind == "Memory64":
                    high = bar.get("RawHigh")
                    if type(high) is not int or not -(1 << 31) <= high <= 0xffffffff:
                        raise ValueError("Invalid PCI BAR high dword")
                    if index >= 5 or raw & 7 != 4:
                        errors.append("Invalid 64-bit BAR encoding")
                    expected = ((high & 0xffffffff) << 32) | (raw & ~15)
                elif kind == "Memory32":
                    if raw & 7:
                        errors.append("Invalid 32-bit BAR encoding")
                    expected = raw & ~15
                elif kind == "IO":
                    if raw & 1 != 1:
                        errors.append("Invalid I/O BAR encoding")
                    expected = raw & ~3
                if expected is not None and address != expected:
                    errors.append("PCI BAR address differs from raw configuration dwords")
                bars.append({"index": index, "kind": kind, "raw_low": f"0x{raw:08x}",
                             "configured_address": hex(address) if type(address) is int else None})
            if not {0, 2, 4, 5}.issubset(indices):
                errors.append("Target BAR configuration is incomplete")
            rom = snapshot.get("VBIOS", {}).get("Status", "Unreported")
            if rom == "NotProvidedByOS":
                notes.append("OS registry supplied no VBIOS; this does not describe live runtime-ROM acquisition")
            devices.append({"pci_identity": "1002:744c" if identity_ok else "Mismatch",
                            "expected_board_matches": board_ok, "bars": bars, "vbios_status": rom})

    device_data = evidence["device.plist"].strip()
    device_items = plistlib.loads(device_data) if device_data else []
    device_absent = device_items == []
    if not device_absent and mode == 'passive':
        errors.append("Experimental device service present during passive-only test")
    capture = live_capture(device_items, errors) if mode == 'live-capture' else None
    notes += ["A loaded diagnostic observer does not prove GPU execution, display control, or Metal",
              "A matching Mach-O UUID associates the load with the selected artifact; it is not a runtime memory hash",
              "Diagnostic snapshots do not establish audio ownership, DMA retirement or engine readiness"]
    if statuses.get("kexts.txt") != 0:
        load_state = "query_failed"
    elif not re.search(re.escape(IDENTIFIER) + r"(?=\s|$)", text("kexts.txt")):
        load_state = "absent_from_loaded_kexts"
    elif uuid_match:
        load_state = "selected_uuid_loaded"
    else:
        load_state = "identity_unverified_or_different"
    notes.append("A package ID or preparation receipt identifies intended media, not the binary actually loaded")
    return {"schema_version": 3, "scope": mode + " diagnostic evidence only",
            "diagnostic_checks_passed": not errors,
            "passive_checks_passed": mode == 'passive' and not errors,
            "live_capture_checks_passed": mode == 'live-capture' and not errors, "macos": version,
            "selected_executable_sha256": binary["sha256"], "selected_macho_uuid": binary["uuid"],
            "loaded_uuid_matches": uuid_match, "experimental_argument_present": experimental,
            "load_state": load_state, "observer_absent": observers == [],
            "experimental_device_absent": device_absent, "devices": devices,
            "live_capture": capture,
            "evidence_sha256": hashes, "errors": errors, "limitations": notes,
            "gpu_execution_verified": False, "display_control_verified": False,
            "metal_verified": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument('--mode', choices=('passive', 'live-capture'), default='passive')
    args = parser.parse_args()
    if args.output.resolve().is_relative_to(args.evidence.resolve()):
        parser.error("Write analysis outside the original evidence folder")
    try:
        result = analyze(args.evidence, args.bundle, args.mode)
    except (OSError, ValueError, TypeError, AttributeError, KeyError) as error:
        parser.exit(2, "Evidence analysis failed: " + str(error) + "\n")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: result[key] for key in
                     ("diagnostic_checks_passed", "passive_checks_passed", "live_capture_checks_passed", "loaded_uuid_matches", "macos", "errors")}, indent=2))
    return 0 if result["diagnostic_checks_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
