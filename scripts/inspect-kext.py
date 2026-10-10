#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate an unsigned Intel Mach-O kext bundle without loading it."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import struct


def inspect(bundle):
    with (bundle / "Contents/Info.plist").open("rb") as stream:
        info = plistlib.load(stream)
    if info.get("CFBundleExecutable") != "HackyNavi" or info.get("CFBundlePackageType") != "KEXT":
        raise ValueError("Unexpected bundle identity/type")
    path = bundle / "Contents/MacOS/HackyNavi"
    data = path.read_bytes()
    if len(data) < 32:
        raise ValueError("Truncated Mach-O header")
    magic, cpu, subtype, kind, count, size, flags, _ = struct.unpack_from("<8I", data)
    if (magic, cpu, kind) != (0xFEEDFACF, 0x01000007, 11):
        raise ValueError("Expected little-endian x86_64 MH_KEXT_BUNDLE")
    limit = 32 + size
    if limit > len(data):
        raise ValueError("Load commands exceed binary")
    cursor = 32
    sections, symbols, undefined = [], {}, []
    minimum_os, sdk = None, None
    for _ in range(count):
        if cursor + 8 > limit:
            raise ValueError("Truncated load command")
        command, length = struct.unpack_from("<2I", data, cursor)
        if length < 8 or length % 8 or cursor + length > limit:
            raise ValueError("Invalid load command size")
        if command in (0xc, 0x18 | 0x80000000, 0x1f | 0x80000000, 0x23 | 0x80000000):
            raise ValueError("Kernel bundle depends on a userspace dynamic library")
        if command == 0x19:
            if length < 72:
                raise ValueError("Short segment command")
            nsects = struct.unpack_from("<I", data, cursor + 64)[0]
            if 72 + nsects * 80 > length:
                raise ValueError("Section array exceeds segment command")
            for i in range(nsects):
                base = cursor + 72 + i * 80
                name = data[base:base + 16].split(b"\0", 1)[0].decode("ascii")
                section_size = struct.unpack_from("<Q", data, base + 40)[0]
                sections.append({"name": name, "bytes": section_size})
        elif command == 2:
            if length < 24:
                raise ValueError("Short symbol table command")
            offset, nsyms, strings, nstrings = struct.unpack_from("<4I", data, cursor + 8)
            if offset + nsyms * 16 > len(data) or strings + nstrings > len(data):
                raise ValueError("Symbol/string table exceeds binary")
            for i in range(nsyms):
                index, typ, section, _, value = struct.unpack_from("<IBBHQ", data, offset + i * 16)
                if index >= nstrings:
                    raise ValueError("Symbol name exceeds strings")
                end = data.find(b"\0", strings + index, strings + nstrings)
                if end < 0:
                    raise ValueError("Unterminated symbol name")
                name = data[strings + index:end].decode("utf-8")
                if typ & 0xe0:  # Debug/STAB entries do not define linker symbols.
                    continue
                defined = (typ & 0x0e) in (2, 0x0e)
                symbols[name] = {"defined": defined, "section": section, "value": value}
                if not defined:
                    undefined.append(name)
        elif command == 0x32:
            if length < 24:
                raise ValueError("Short build-version command")
            platform, minimum_os, sdk = struct.unpack_from("<3I", data, cursor + 8)
            if platform != 1:
                raise ValueError("Expected macOS target")
        elif command == 0x24:
            if length < 16:
                raise ValueError("Short minimum-version command")
            minimum_os, sdk = struct.unpack_from("<2I", data, cursor + 8)
        cursor += length
    if cursor != limit:
        raise ValueError("Command count does not match the load-command extent")
    # Apple's kext linker omits LC_BUILD_VERSION on this target. A command,
    # when present, must agree with the build's explicit deployment flags.
    if minimum_os is not None and minimum_os != 0x000f0000:
        raise ValueError("macOS15 deployment baseline mismatch")
    for name in ("__start", "__stop", "_kmod_info", "__realmain", "__antimain"):
        if not symbols.get(name, {}).get("defined"):
            raise ValueError(f"Required definition absent: {name}")
    if not any(s["name"] == "__mod_init_func" and s["bytes"] >= 8 for s in sections):
        raise ValueError("C++ module initializers are missing")
    project_undefined = [s for s in undefined if "hackynavi" in s.lower()]
    if project_undefined:
        raise ValueError(f"Unlinked project definitions: {project_undefined}")
    return {"format": "MH_KEXT_BUNDLE", "architecture": "x86_64", "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(), "cpu_subtype": subtype, "flags": flags,
            "minimum_os_raw": minimum_os, "sdk_raw": sdk, "sections": sections,
            "module_symbols": {s: symbols[s] for s in ("__start", "__stop", "_kmod_info", "__realmain", "__antimain")},
            "undefined_kernel_symbols": undefined, "bundle_identifier": info["CFBundleIdentifier"],
            "dependencies": info["OSBundleLibraries"], "loaded": False, "hardware_tested": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = inspect(args.bundle)
    rendered = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    print(f"Verified x86_64 MH_KEXT_BUNDLE: {result['bytes']:,} bytes, module entry points and C++ initializers present")


if __name__ == "__main__":
    main()
