# SPDX-License-Identifier: MIT
"""Bounded fragment following measure-accelerator-kc-metadata's helpers.

The local runner supplies EXPECTED_BOOT and TARGET_OFFSETS from the measured
format-11 chain pointers. Read only the kernel's nlists and names whose exact
values match those targets. No running kernel access or remote file creation.
"""
if not 0 < len(TARGET_OFFSETS) <= 512 or len(set(TARGET_OFFSETS)) != len(TARGET_OFFSETS):
    raise ValueError("Exact target count bound")
path = Path("/System/Library/KernelCollections/BootKernelExtensions.kc")
result = {"path": str(path), "remote_files_created": False, "running_kernel_read": False,
          "gpu_submitted": False, "kernel_loaded": False, "selected_symbols": [],
          "requested_target_offsets": TARGET_OFFSETS, "name_bytes_read": 0}
with path.open("rb") as file:
    size = path.stat().st_size
    info, commands = header(file, 0, size)
    uid = next(str(uuid.UUID(bytes=data[8:24])) for kind, data in commands if kind == 0x1B)
    if info["header_sha256"] != EXPECTED_BOOT["header_sha256"] or uid != EXPECTED_BOOT["uuid"]:
        raise ValueError("Boot collection changed")
    entries = []
    for kind, data in commands:
        if kind != 0x80000035 or len(data) < 32:
            continue
        va, offset, nameoff, reserved = struct.unpack_from("<QQII", data, 8)
        if nameoff < 32 or nameoff >= len(data):
            raise ValueError("Fileset name bound")
        name = data[nameoff:].split(b"\0", 1)[0].decode()
        if name == "com.apple.kernel":
            entries.append({"identifier": name, "vmaddr": va, "file_offset": offset})
    if len(entries) != 1:
        raise ValueError("Expected one kernel entry")
    # This is the base independently solved from three actual kernel symbols
    # and their captured pointer targets. Neither the enclosing collection
    # nor its kernel entry's header address is assumed to be this base.
    base = EXPECTED_KERNEL_CACHE_BASE
    kernel, commands = header(file, entries[0]["file_offset"], size)
    kernel.update(entries[0])
    kernel["uuid"] = next(str(uuid.UUID(bytes=data[8:24])) for kind, data in commands if kind == 0x1B)
    symtabs = [data for kind, data in commands if kind == 2 and len(data) == 24]
    if len(symtabs) != 1:
        raise ValueError("Expected kernel symbol table")
    symoff, count, stroff, strbytes = struct.unpack_from("<IIII", symtabs[0], 8)
    if not count or count > 262144 or stroff + strbytes > size:
        raise ValueError("Kernel symbol-table bound")
    symbols = read_at(file, symoff, count * 16, size)
    targets = {base + offset for offset in TARGET_OFFSETS}
    names_digest = hashlib.sha256()
    for i in range(count):
        strx, kind, section, descriptor, value = struct.unpack_from("<IBBHQ", symbols, i * 16)
        if value not in targets or not section:
            continue
        if strx >= strbytes:
            raise ValueError("Symbol name offset bound")
        prefix = read_at(file, stroff + strx, min(1024, strbytes - strx), size)
        if b"\0" not in prefix:
            raise ValueError("Exact symbol name length bound")
        result["name_bytes_read"] += len(prefix)
        if result["name_bytes_read"] > 524288:
            raise ValueError("Total selected name read bound")
        names_digest.update(struct.pack("<I", strx) + prefix)
        result["selected_symbols"].append({"name": prefix.split(b"\0", 1)[0].decode(),
            "value": value, "target_offset": value - base, "type": kind, "section": section})
    result.update(collection_uuid=uid, collection_header_sha256=info["header_sha256"],
        collection_base=EXPECTED_BOOT["collection_base"], kernel_cache_base=base,
        kernel=kernel, symbol_table={"symoff": symoff, "count": count,
        "stroff": stroff, "strbytes": strbytes}, symbol_data_sha256=hashlib.sha256(symbols).hexdigest(),
        name_reads_sha256=names_digest.hexdigest())
print(json.dumps(result, indent=2, sort_keys=True))
