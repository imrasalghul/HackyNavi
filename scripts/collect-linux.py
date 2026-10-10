#!/usr/bin/env python3
"""Collect a bounded, read-only Linux sysfs baseline for a Navi 31 PCI device.

Only existing text attributes are read. This does not enable/read the PCI ROM,
open resourceN mappings, touch config space, or access debugfs/MMIO. Output is
a new JSON file, never an overwritten file. Python 3.9+; no extra packages.
"""

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import stat
import sys


BDF_RE = re.compile(r"^[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[01][0-9a-fA-F]\.[0-7]$")
MAX_ATTRIBUTE_BYTES = 8192
MAX_TOTAL_BYTES = 1024 * 1024
MAX_FILES = 2048
MAX_ENTRIES = 4096
MAX_DEPTH = 6
IDENTITY_FIELDS = ("vendor", "device", "subsystem_vendor", "subsystem_device", "revision", "class")
OPTIONAL_FIELDS = (
    "mem_info_vram_total", "mem_info_vis_vram_total", "mem_info_gtt_total",
    "mem_info_vram_used", "mem_info_vis_vram_used", "mem_info_gtt_used",
    "current_link_speed", "current_link_width", "max_link_speed", "max_link_width",
    "vbios_version", "unique_id",
)


class CollectionError(Exception):
    """An input, selection, or required identity error."""


def within(path, root):
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def bounded_entries(path, limit=MAX_ENTRIES):
    """Bound directory enumeration itself, not just subsequent file reads."""
    entries = []
    with os.scandir(path) as iterator:
        for entry in iterator:
            if len(entries) >= limit:
                raise CollectionError("directory entry limit exceeded: " + str(path))
            entries.append(Path(entry.path))
    return sorted(entries, key=lambda entry: entry.name)


class Reader:
    def __init__(self, root):
        self.root = root.resolve(strict=True)
        self.warnings = []
        self.files = 0
        self.bytes = 0

    def warn(self, path, reason):
        try:
            label = str(path.relative_to(self.root))
        except ValueError:
            label = str(path)
        self.warnings.append({"path": label.replace("\\", "/"), "reason": reason})

    def text(self, path, required=False):
        try:
            if self.files >= MAX_FILES or self.bytes >= MAX_TOTAL_BYTES:
                raise CollectionError("collection read limit reached")
            # Actual /sys/bus/pci/devices entries are symlinks, but their selected
            # target becomes root. Attribute and recursive symlinks are rejected.
            if path.is_symlink() or not within(path.resolve(strict=True), self.root):
                raise CollectionError("attribute symlink or path outside selected device")
            if not stat.S_ISREG(path.stat().st_mode):
                raise CollectionError("attribute is not a regular text file")
            self.files += 1
            with path.open("rb") as stream:
                data = stream.read(min(MAX_ATTRIBUTE_BYTES, MAX_TOTAL_BYTES - self.bytes) + 1)
            self.bytes += len(data)
            if len(data) > MAX_ATTRIBUTE_BYTES or self.bytes > MAX_TOTAL_BYTES:
                raise CollectionError("attribute exceeds collection size limit")
            return data.decode("ascii").strip()
        except (OSError, UnicodeError, CollectionError) as exc:
            if required:
                raise CollectionError(f"cannot read required attribute {path.name}: {exc}") from exc
            self.warn(path, str(exc))
            return None


def select_device(sysfs_root, requested=None):
    if requested is not None and not BDF_RE.fullmatch(requested):
        raise CollectionError("device must be a full PCI BDF, for example 0000:03:00.0")
    pci_root = sysfs_root / "bus" / "pci" / "devices"
    if not within(pci_root.resolve(strict=True), sysfs_root):
        raise CollectionError("PCI device directory resolves outside sysfs root")
    entries = [pci_root / requested.lower()] if requested else bounded_entries(pci_root)
    matches = []
    for entry in entries:
        if not BDF_RE.fullmatch(entry.name):
            continue
        try:
            resolved = entry.resolve(strict=True)
            if not within(resolved, sysfs_root) or not resolved.is_dir():
                raise CollectionError("PCI device resolves outside sysfs root or is not a directory")
            reader = Reader(resolved)
            vendor = int(reader.text(resolved / "vendor", required=True), 16)
            device = int(reader.text(resolved / "device", required=True), 16)
            if (vendor, device) == (0x1002, 0x744C):
                matches.append((entry.name.lower(), resolved))
            elif requested:
                raise CollectionError("selected device is not AMD 1002:744c")
        except (OSError, ValueError, CollectionError) as exc:
            if requested:
                raise CollectionError(str(exc)) from exc
            # Unreadable unrelated PCI entries cannot be identified as candidates.
            continue
    if not matches:
        raise CollectionError("no readable AMD 1002:744c PCI device found")
    if len(matches) != 1:
        raise CollectionError("multiple AMD 1002:744c devices found; select --device from " +
                              ", ".join(bdf for bdf, _ in matches))
    return matches[0]


def collect_discovery(reader):
    directory = reader.root / "ip_discovery"
    result = {}
    visited = 0

    def visit(path, depth):
        nonlocal visited
        try:
            if path.is_symlink() or not within(path.resolve(strict=True), reader.root):
                raise CollectionError("discovery symlinks are not followed")
            if depth > MAX_DEPTH:
                raise CollectionError("discovery recursion limit exceeded")
            entries = bounded_entries(path, MAX_ENTRIES - visited)
            visited += len(entries)
        except (OSError, CollectionError) as exc:
            reader.warn(path, str(exc))
            return
        for entry in entries:
            try:
                if entry.is_symlink():
                    reader.warn(entry, "discovery symlinks are not followed")
                elif entry.is_dir():
                    visit(entry, depth + 1)
                else:
                    value = reader.text(entry)
                    if value is not None:
                        result[entry.relative_to(directory).as_posix()] = value
            except OSError as exc:
                reader.warn(entry, str(exc))

    visit(directory, 0)
    return result


def collect(sysfs_root, requested=None):
    sysfs_root = Path(sysfs_root).resolve(strict=True)
    bdf, device_root = select_device(sysfs_root, requested)
    reader = Reader(device_root)
    identity = {}
    for field in IDENTITY_FIELDS:
        value = reader.text(device_root / field, required=field in ("vendor", "device"))
        if value is not None:
            try:
                identity[field] = int(value, 16)
            except ValueError:
                reader.warn(device_root / field, "invalid hexadecimal identity value")
                if field in ("vendor", "device"):
                    raise CollectionError("invalid required PCI identity")
    if (identity.get("vendor"), identity.get("device")) != (0x1002, 0x744C):
        raise CollectionError("PCI identity changed during collection")

    driver_path = device_root / "driver"
    try:
        if driver_path.is_symlink():
            target = driver_path.resolve(strict=True)
            if not within(target, sysfs_root / "bus" / "pci" / "drivers"):
                raise CollectionError("driver symlink target is outside PCI drivers directory")
            driver = {"state": "bound", "name": target.name}
        elif driver_path.exists():
            raise CollectionError("driver attribute is not a symlink")
        else:
            driver = {"state": "unbound", "name": None}
            reader.warn(driver_path, "no driver bound; amdgpu attributes may be absent")
    except (OSError, CollectionError) as exc:
        driver = {"state": "unavailable", "name": None}
        reader.warn(driver_path, str(exc))

    resource = reader.text(device_root / "resource")
    attributes = {}
    for field in OPTIONAL_FIELDS:
        value = reader.text(device_root / field)
        if value is not None:
            attributes[field] = value
    discovery = collect_discovery(reader)
    if driver.get("name") not in (None, "amdgpu"):
        reader.warn(driver_path, "device is bound to a driver other than amdgpu")
    return {
        "schema_version": 1,
        "collected_at_utc": datetime.now(timezone.utc).isoformat(),
        "scope": "Existing Linux sysfs text attributes only; no device writes or direct hardware access.",
        "pci_bdf": bdf,
        "identity": identity,
        "driver": driver,
        "resource_raw": resource,
        "attributes": attributes,
        "ip_discovery": discovery,
        "limits": {"attribute_bytes": MAX_ATTRIBUTE_BYTES, "total_bytes": MAX_TOTAL_BYTES,
                   "files": MAX_FILES, "entries_per_tree": MAX_ENTRIES, "depth": MAX_DEPTH},
        "read_stats": {"files": reader.files, "bytes": reader.bytes},
        "warnings": reader.warnings,
        "notes": [
            "Text values are preserved as exported, including numeric units and base-address format.",
            "resource_raw describes assigned PCI resources, not a probe of BAR sizes or usable VRAM.",
            "IP discovery values are the running Linux driver's exported interpretation, not a raw firmware binary.",
            "The snapshot is not atomic; device power state, memory usage, and driver binding may change during collection.",
        ],
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", help="full PCI BDF; required when multiple 1002:744c devices exist")
    parser.add_argument("--sysfs-root", default="/sys", type=Path, help="sysfs mount root (default: /sys)")
    parser.add_argument("--output", required=True, type=Path, help="new JSON output file; existing files are refused")
    args = parser.parse_args(argv)
    try:
        sysfs_root = args.sysfs_root.resolve(strict=True)
        output = args.output.resolve(strict=False)
        if within(output, sysfs_root):
            raise CollectionError("output must be outside the sysfs root")
        if args.output.exists() or args.output.is_symlink():
            raise CollectionError("output already exists; choose a new filename")
        result = collect(sysfs_root, args.device)
        with args.output.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(result, stream, indent=2, sort_keys=True)
            stream.write("\n")
    except (OSError, CollectionError) as exc:
        print("collect-linux: " + str(exc), file=sys.stderr)
        return 1
    print(f"Collected {result['pci_bdf']} to {args.output} ({len(result['warnings'])} warnings)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
