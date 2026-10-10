#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read bounded metadata for two named family entries in standard collections.

No kernel load, sysctl, memory read, GPU call or remote file creation. This
reads on-disk Mach-O metadata, not a running kernel's class registry.
"""
import hashlib
import json
from pathlib import Path
import platform
import struct
import subprocess
import uuid

if platform.system() != "Darwin":
    raise SystemExit("Requires actual macOS collection files")

names = {"com.apple.iokit.IOAcceleratorFamily2", "com.apple.iokit.IOGraphicsFamily"}
classes = ("IOGraphicsAccelerator2", "IOAccelerator", "IOAccelDevice2", "IOAccelSharedUserClient2",
           "IOAccelEventMachineFast2", "IOAccelTask", "IOAccelDisplayMachine")


def read_at(file, offset, length, file_size):
    if offset < 0 or length < 0 or length > 4 * 1024 * 1024 or offset + length > file_size:
        raise ValueError("Bounded file range rejected")
    file.seek(offset)
    data = file.read(length)
    if len(data) != length:
        raise ValueError("Short metadata read")
    return data


def header(file, offset, file_size):
    raw = read_at(file, offset, 32, file_size)
    magic, cpu, subtype, filetype, count, length, flags, reserved = struct.unpack("<IiiIIIII", raw)
    if magic != 0xFEEDFACF or cpu != 0x1000007 or count > 1024 or length > 1048576:
        raise ValueError("Unexpected Mach-O header")
    data = read_at(file, offset + 32, length, file_size)
    commands, cursor = [], 0
    for _ in range(count):
        if cursor + 8 > len(data):
            raise ValueError("Command header exceeds range")
        kind, size = struct.unpack_from("<II", data, cursor)
        if size < 8 or cursor + size > len(data):
            raise ValueError("Command exceeds range")
        commands.append((kind, data[cursor:cursor + size]))
        cursor += size
    if cursor != len(data):
        raise ValueError("Load-command length mismatch")
    return {"file_offset": offset, "filetype": filetype, "cpu": cpu,
            "header_bytes": 32 + length, "header_sha256": hashlib.sha256(raw + data).hexdigest()}, commands


def inspect_entry(file, entry, file_size):
    info, commands = header(file, entry["file_offset"], file_size)
    info.update(entry)
    info["segments"] = []
    symtab = None
    for kind, data in commands:
        if kind == 0x1B and len(data) == 24:
            info["uuid"] = str(uuid.UUID(bytes=data[8:24]))
        elif kind == 0x19 and len(data) >= 72:
            name, va, vmbytes, fileoff, filebytes, maxprot, initprot, nsects, flags = struct.unpack_from("<16sQQQQiiII", data, 8)
            info["segments"].append({"name": name.rstrip(b"\0").decode(), "vmaddr": va, "vmsize": vmbytes,
                                      "file_offset": fileoff, "file_bytes": filebytes, "sections": nsects})
        elif kind == 2 and len(data) == 24:
            symoff, count, stroff, strbytes = struct.unpack_from("<IIII", data, 8)
            symtab = {"symoff": symoff, "count": count, "stroff": stroff, "strbytes": strbytes}
            info["symbol_table"] = symtab
    if symtab:
        if symtab["count"] > 8192 or symtab["stroff"] + symtab["strbytes"] > file_size:
            info["symbols_skipped"] = "symbol count/string-table file bounds"
        else:
            symbols = read_at(file, symtab["symoff"], symtab["count"] * 16, file_size)
            selected = []
            names_digest = hashlib.sha256()
            info["symbol_data_sha256"] = hashlib.sha256(symbols).hexdigest()
            info["name_bytes_read"] = 0
            for i in range(symtab["count"]):
                strx, kind, section, descriptor, value = struct.unpack_from("<IBBHQ", symbols, i * 16)
                if strx >= symtab["strbytes"]:
                    raise ValueError("Symbol string index out of range")
                # Collections share a large string pool. Read only a bounded
                # prefix at each symbol belonging to these two named entries,
                # not the whole collection's shared string table.
                prefix = read_at(file, symtab["stroff"] + strx, min(256, symtab["strbytes"] - strx), file_size)
                info["name_bytes_read"] += len(prefix)
                names_digest.update(struct.pack("<I", strx) + prefix)
                if not any(cls.encode() in prefix.split(b"\0", 1)[0] for cls in classes):
                    continue
                data = prefix
                if b"\0" not in data:
                    data = read_at(file, symtab["stroff"] + strx, min(1024, symtab["strbytes"] - strx), file_size)
                    info["name_bytes_read"] += len(data)
                    names_digest.update(data)
                if b"\0" not in data:
                    raise ValueError("Selected symbol name exceeds bound")
                name = data.split(b"\0", 1)[0].decode("utf-8", errors="replace")
                if any(cls in name for cls in classes):
                    selected.append({"name": name, "type": kind, "section": section, "value": value})
                if len(selected) > 2048:
                    raise ValueError("Selected-symbol bound")
            info["name_reads_sha256"] = names_digest.hexdigest()
            info["selected_symbols"] = selected
    return info


report = {"system": subprocess.check_output(["sw_vers"], text=True, timeout=10).strip(),
          "collections": {}, "kernel_loaded": False, "running_kernel_read": False,
          "gpu_submitted": False, "remote_files_created": False,
          "limits": {"collections": 2, "entries_per_collection": 2, "load_commands_bytes": 1048576,
                     "symbol_count": 8192, "name_prefix_bytes": 256, "selected_name_bytes": 1024,
                     "selected_symbols": 2048}}
for basename in ("SystemKernelExtensions.kc", "BootKernelExtensions.kc"):
    path = Path("/System/Library/KernelCollections") / basename
    item = {"exists": path.is_file(), "entries": []}
    report["collections"][str(path)] = item
    if not path.is_file():
        continue
    size = path.stat().st_size
    item["file_bytes"] = size
    try:
        with path.open("rb") as file:
            metadata, commands = header(file, 0, size)
            item.update(metadata)
            for kind, data in commands:
                if kind == 0x1B and len(data) == 24:
                    item["uuid"] = str(uuid.UUID(bytes=data[8:24]))
                if kind != 0x80000035 or len(data) < 32:
                    continue
                va, offset, nameoff, reserved = struct.unpack_from("<QQII", data, 8)
                if nameoff < 32 or nameoff >= len(data):
                    raise ValueError("Invalid fileset identifier offset")
                name = data[nameoff:].split(b"\0", 1)[0].decode()
                if name in names:
                    if len(item["entries"]) >= 2:
                        raise ValueError("Too many matching entries")
                    item["entries"].append(inspect_entry(file, {"identifier": name, "vmaddr": va, "file_offset": offset}, size))
    except (OSError, ValueError, StopIteration) as error:
        item["error"] = str(error)
print(json.dumps(report, indent=2, sort_keys=True))
