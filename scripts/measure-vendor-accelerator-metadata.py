# SPDX-License-Identifier: MIT
"""Bounded fragment following the kernel-collection metadata reader helpers.

REQUESTED_VENDOR_ENTRIES comes from the captured collection's fileset names.
Only accelerator symbols from two exact shipped vendor entries are returned.
This reads disk data; it never loads a kext or accesses a running GPU.
"""
if len(REQUESTED_VENDOR_ENTRIES) != 2:
    raise ValueError("Require exactly the two selected vendor entries")
path = Path("/System/Library/KernelCollections/SystemKernelExtensions.kc")
result = {"collection": str(path), "entries": [], "remote_files_created": False,
          "gpu_submitted": False, "kernel_loaded": False, "running_kernel_read": False,
          "total_name_bytes_read": 0, "total_nlist_bytes_read": 0}
with path.open("rb") as file:
    size = path.stat().st_size
    info, commands = header(file, 0, size)
    uid = next(str(uuid.UUID(bytes=data[8:24])) for kind, data in commands if kind == 0x1B)
    if uid != EXPECTED_SYSTEM["uuid"] or info["header_sha256"] != EXPECTED_SYSTEM["header_sha256"]:
        raise ValueError("Measured collection changed")
    result["collection_metadata"] = {**info, "uuid": uid}
    entries = {}
    for kind, data in commands:
        if kind != 0x80000035 or len(data) < 32:
            continue
        va, off, no, reserved = struct.unpack_from("<QQII", data, 8)
        if no < 32 or no >= len(data):
            raise ValueError("Fileset string bound")
        name = data[no:].split(b"\0", 1)[0].decode()
        if name in REQUESTED_VENDOR_ENTRIES:
            entries[name] = {"identifier": name, "vmaddr": va, "file_offset": off}
    if set(entries) != set(REQUESTED_VENDOR_ENTRIES):
        raise ValueError("Selected vendor filesets missing")
    for name, entry in entries.items():
        if entry["file_offset"] != REQUESTED_VENDOR_ENTRIES[name]["file_offset"]:
            raise ValueError("Selected fileset offset changed")
        item, entry_commands = header(file, entry["file_offset"], size)
        item.update(entry)
        item["uuid"] = next(str(uuid.UUID(bytes=data[8:24])) for kind, data in entry_commands if kind == 0x1B)
        symtabs = [data for kind, data in entry_commands if kind == 2 and len(data) == 24]
        if len(symtabs) != 1:
            raise ValueError("One vendor symbol table required")
        symoff, count, stroff, strbytes = struct.unpack_from("<IIII", symtabs[0], 8)
        if not 0 < count <= 65536 or stroff + strbytes > size:
            raise ValueError("Vendor symbol-table bound")
        symbols = read_at(file, symoff, count * 16, size)
        result["total_nlist_bytes_read"] += len(symbols)
        if result["total_nlist_bytes_read"] > 2 * 1024 * 1024:
            raise ValueError("Combined nlist read bound")
        names_digest = hashlib.sha256()
        selected = []
        for i in range(count):
            strx, kind, section, descriptor, value = struct.unpack_from("<IBBHQ", symbols, i * 16)
            if not section or kind & 0xE0 or kind & 0x0E != 0x0E:
                continue
            if strx >= strbytes:
                raise ValueError("Vendor symbol name index bound")
            data = read_at(file, stroff + strx, min(128, strbytes - strx), size)
            result["total_name_bytes_read"] += len(data)
            if result["total_name_bytes_read"] > 8 * 1024 * 1024:
                raise ValueError("Combined name read bound")
            names_digest.update(struct.pack("<I", strx) + data)
            if b"Accelerator" not in data.split(b"\0", 1)[0]:
                continue
            if b"\0" not in data:
                data = read_at(file, stroff + strx, min(1024, strbytes - strx), size)
                result["total_name_bytes_read"] += len(data)
                names_digest.update(data)
            if b"\0" not in data:
                raise ValueError("Selected accelerator name exceeds bound")
            if result["total_name_bytes_read"] > 8 * 1024 * 1024:
                raise ValueError("Combined name read bound")
            selected.append({"name": data.split(b"\0", 1)[0].decode(), "value": value,
                             "type": kind, "section": section})
            if len(selected) > 4096:
                raise ValueError("Selected accelerator symbol count bound")
        item.update(symbol_table={"symoff": symoff, "count": count, "stroff": stroff, "strbytes": strbytes},
                    symbol_data_sha256=hashlib.sha256(symbols).hexdigest(),
                    name_reads_sha256=names_digest.hexdigest(), selected_symbols=selected)
        result["entries"].append(item)
result["limits"] = {"entries": 2, "nlist_bytes_total": 2097152, "name_bytes_total": 8388608,
                    "selected_symbols_per_entry": 4096, "name_prefix_bytes": 128, "selected_name_bytes": 1024}
print(json.dumps(result, indent=2, sort_keys=True))
