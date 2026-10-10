# SPDX-License-Identifier: MIT
"""Bounded follow-up fragment using measure-accelerator-kc-metadata helpers.

The local runner embeds EXPECTED_COLLECTIONS and REQUESTS from the recorded
symbol inventory. Reads only exact tables/constructors and their chain pages.
"""
if len(REQUESTS) > 6:
    raise RuntimeError("Requested table/constructor count bound")


def segments_of(commands):
    result = []
    for kind, data in commands:
        if kind == 0x19 and len(data) >= 72:
            name, va, vmbytes, fileoff, filebytes, maxprot, initprot, nsects, flags = struct.unpack_from("<16sQQQQiiII", data, 8)
            result.append({"name": name.rstrip(b"\0").decode(), "vmaddr": va, "vmsize": vmbytes,
                           "file_offset": fileoff, "file_bytes": filebytes, "protection": initprot})
    return result


def image_uuid(commands):
    return next(str(uuid.UUID(bytes=data[8:24])) for kind, data in commands if kind == 0x1B and len(data) == 24)


def file_address(segments, va, count):
    segment = next((s for s in segments if s["vmaddr"] <= va and va + count <= s["vmaddr"] + s["file_bytes"]), None)
    if not segment:
        raise RuntimeError("Requested VM range has no bounded file-backed segment")
    return segment["file_offset"] + va - segment["vmaddr"]


result = {"system": subprocess.check_output(["sw_vers"], text=True, timeout=10).strip(),
          "collections": {}, "ranges": {}, "chain_pages": {},
          "constructor_references": {},
          "kernel_loaded": False, "running_kernel_read": False,
          "gpu_submitted": False, "remote_files_created": False}
for path_string, expected in EXPECTED_COLLECTIONS.items():
    path = Path(path_string)
    with path.open("rb") as file:
        file_size = path.stat().st_size
        root_info, commands = header(file, 0, file_size)
        root_uuid = image_uuid(commands)
        if root_uuid != expected["uuid"] or root_info["header_sha256"] != expected["header_sha256"]:
            raise RuntimeError("Kernel collection changed since inventory")
        root_segments = segments_of(commands)
        base = next(s["vmaddr"] for s in root_segments if s["file_offset"] == 0 and s["file_bytes"])
        root_info.update(uuid=root_uuid, segments=root_segments, collection_base=base)
        result["collections"][path_string] = root_info
        needed = [r for r in REQUESTS if r["collection"] == path_string]
        if not needed:
            continue
        entries = {}
        for kind, data in commands:
            if kind != 0x80000035 or len(data) < 32:
                continue
            va, offset, nameoff, reserved = struct.unpack_from("<QQII", data, 8)
            if nameoff < 32 or nameoff >= len(data):
                raise RuntimeError("Invalid fileset identifier")
            name = data[nameoff:].split(b"\0", 1)[0].decode()
            if any(r["entry"] == name for r in needed):
                e_info, e_commands = header(file, offset, file_size)
                pinned = next(e for e in expected["entries"] if e["identifier"] == name)
                if e_info["header_sha256"] != pinned["header_sha256"] or image_uuid(e_commands) != pinned["uuid"]:
                    raise RuntimeError("Family entry changed since inventory")
                sym = next(data for kind, data in e_commands if kind == 2)
                symoff, symcount, stroff, strbytes = struct.unpack_from("<IIII", sym, 8)
                if symcount > 8192:
                    raise RuntimeError("Symbol count bound")
                symdata = read_at(file, symoff, symcount * 16, file_size)
                if hashlib.sha256(symdata).hexdigest() != pinned["symbol_data_sha256"]:
                    raise RuntimeError("Family symbol records changed")
                defined = [struct.unpack_from("<IBBHQ", symdata, i * 16) for i in range(symcount)]
                defined = [s for s in defined if not s[1] & 0xE0 and s[1] & 0x0E == 0x0E and s[2] and s[4]]
                entries[name] = {"segments": segments_of(e_commands), "symbols": defined, "stroff": stroff,
                                 "strbytes": strbytes, "uuid": pinned["uuid"], "commands": e_commands,
                                 "base": va}
        pointer_addresses = set()
        for request in needed:
            entry = entries[request["entry"]]
            address = request["value"]
            candidates = [s for s in entry["symbols"] if s[4] == address]
            actual_names = []
            for s in candidates:
                if s[0] >= entry["strbytes"]:
                    raise RuntimeError("Invalid symbol string index")
                data = read_at(file, entry["stroff"] + s[0], min(256, entry["strbytes"] - s[0]), file_size)
                actual_names.append(data.split(b"\0", 1)[0].decode())
            if request["symbol"] not in actual_names:
                raise RuntimeError("Requested symbol/address mismatch")
            end = min(s[4] for s in entry["symbols"] if s[4] > address)
            maximum = 4096 if request["kind"] == "vtable" else 1024
            count = min(end - address, maximum)
            if request["kind"] == "vtable" and (count != end - address or count % 8):
                raise RuntimeError("Vtable range is not completely bounded/aligned")
            offset = file_address(entry["segments"], address, count)
            data = read_at(file, offset, count, file_size)
            result["ranges"][request["symbol"]] = dict(request, file_offset=offset, bytes=count, next_symbol_address=end,
                                                       truncated=count != end - address, hex=data.hex(),
                                                       sha256=hashlib.sha256(data).hexdigest(), entry_uuid=entry["uuid"])
            if request["kind"] == "vtable":
                pointer_addresses.update(range(address, address + count, 8))
            elif (len(data) == 50 and data[9:12] == b"\x48\x8d\x35" and data[16:19] == b"\x48\x8b\x15"
                  and data[23] == 0xB9 and data[28] == 0xE8):
                name_address = address + 16 + int.from_bytes(data[12:16], "little", signed=True)
                parent_slot = address + 23 + int.from_bytes(data[19:23], "little", signed=True)
                call_target = address + 33 + int.from_bytes(data[29:33], "little", signed=True)
                name_bytes = read_at(file, file_address(root_segments, name_address, 128), 128, file_size)
                parent_bytes = read_at(file, file_address(root_segments, parent_slot, 8), 8, file_size)
                stub = read_at(file, file_address(root_segments, call_target, 16), 16, file_size)
                refs = {"class_name_address": name_address, "class_name": name_bytes.split(b"\0", 1)[0].decode(),
                        "size_argument_ecx": int.from_bytes(data[24:28], "little"),
                        "parent_pointer_slot": parent_slot, "parent_pointer_hex": parent_bytes.hex(),
                        "call_target": call_target, "call_stub_hex": stub.hex()}
                pointer_addresses.add(parent_slot)
                if stub[:2] == b"\xff\x25":
                    stub_slot = call_target + 6 + int.from_bytes(stub[2:6], "little", signed=True)
                    refs["call_pointer_slot"] = stub_slot
                    refs["call_pointer_hex"] = read_at(file, file_address(root_segments, stub_slot, 8), 8, file_size).hex()
                    pointer_addresses.add(stub_slot)
                result["constructor_references"][request["symbol"]] = refs

        fixup_images = [("collection", commands, base)] + [(name, value["commands"], value["base"]) for name, value in entries.items()]
        for source_name, source_commands, source_base in fixup_images:
            fixup_command = next((data for kind, data in source_commands if kind == 0x80000034), None)
            if fixup_command is None:
                continue
            dataoff, datasize = struct.unpack_from("<II", fixup_command, 8)
            if datasize < 28 or dataoff + datasize > file_size:
                raise RuntimeError("Fixup payload out of file bounds")
            fixup_header = read_at(file, dataoff, 28, file_size)
            version, starts_offset, imports_offset, symbols_offset, imports_count, imports_format, symbols_format = struct.unpack("<7I", fixup_header)
            if version != 0 or starts_offset + 4 > datasize:
                raise RuntimeError("Unsupported fixup header")
            starts_base = dataoff + starts_offset
            segcount = struct.unpack("<I", read_at(file, starts_base, 4, file_size))[0]
            if segcount > 1024 or starts_offset + 4 + segcount * 4 > datasize:
                raise RuntimeError("Fixup segment count bound")
            offsets = struct.unpack("<" + "I" * segcount, read_at(file, starts_base + 4, segcount * 4, file_size))
            fixup_info = {"file_offset": dataoff, "bytes": datasize, "header_hex": fixup_header.hex(), "seg_count": segcount,
                                    "starts_offset": starts_offset, "formats_encountered": []}
            root_info.setdefault("fixup_images", {})[source_name] = fixup_info
            for segindex, relative in enumerate(offsets):
                if not relative:
                    continue
                if starts_offset + relative + 22 > datasize:
                    raise RuntimeError("Fixup segment header exceeds payload")
                raw = read_at(file, starts_base + relative, 22, file_size)
                size, page_size, fmt, segment_offset, maximum, page_count = struct.unpack("<IHHQIH", raw)
                if size < 22 + page_count * 2 or starts_offset + relative + size > datasize or page_size not in (4096, 16384):
                    raise RuntimeError("Fixup segment/page bounds")
                segment_va = source_base + segment_offset
                relevant_pages = set()
                for va in pointer_addresses:
                    if segment_va <= va < segment_va + page_count * page_size:
                        relevant_pages.add((va - segment_va) // page_size)
                for page in sorted(relevant_pages):
                    if len(result["chain_pages"]) >= 8:
                        raise RuntimeError("Chain-page count bound")
                    start = struct.unpack("<H", read_at(file, starts_base + relative + 22 + 2 * page, 2, file_size))[0]
                    if fmt != 11 or start >= 0x8000:
                        raise RuntimeError("Unsupported actual x86 chain format or multipage start")
                    va = segment_va + page * page_size
                    offset = file_address(root_segments, va, page_size)
                    data = read_at(file, offset, page_size, file_size)
                    result["chain_pages"][str(va)] = {"vmaddr": va, "file_offset": offset, "bytes": page_size,
                        "first_chain_offset": start, "pointer_format": fmt, "segment_index": segindex,
                        "segment_offset": segment_offset, "fixup_image": source_name, "segment_header_hex": raw.hex(),
                        "hex": data.hex(), "sha256": hashlib.sha256(data).hexdigest()}
                    if fmt not in fixup_info["formats_encountered"]:
                        fixup_info["formats_encountered"].append(fmt)

result["limits"] = {"requested_ranges": 6, "vtable_bytes_each": 4096, "constructor_bytes_each": 1024,
                    "chain_pages": 8, "page_bytes_each": 16384, "symbol_count_per_entry": 8192}
print(json.dumps(result, indent=2, sort_keys=True))
