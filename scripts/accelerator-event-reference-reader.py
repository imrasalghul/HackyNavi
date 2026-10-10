# SPDX-License-Identifier: MIT
"""Helpers for exact observed event-call references, never function execution."""


def capture_event_stubs(file, file_size, segments, result, pointer_addresses):
    if len(EVENT_STUBS) > 12:
        raise ValueError("Event stub count bound")
    for address in EVENT_STUBS:
        data = read_at(file, file_address(segments, address, 6), 6, file_size)
        if data[:2] != b"\xff\x25":
            # The two observed AMD calls may name real internal functions,
            # rather than imports. Preserve the bytes and resolve that exact
            # function address without pretending it is a chained pointer.
            result.setdefault("event_direct_calls", {})[str(address)] = {
                "address": address, "hex": data.hex(), "indirect_stub": False}
            continue
        slot = address + 6 + int.from_bytes(data[2:], "little", signed=True)
        raw = read_at(file, file_address(segments, slot, 8), 8, file_size)
        result.setdefault("event_stubs", {})[str(address)] = {
            "address": address, "hex": data.hex(), "pointer_slot": slot, "pointer_hex": raw.hex()}
        pointer_addresses.add(slot)


def resolve_event_targets(result):
    words = []
    for request in EVENT_WORDS:
        item = result["ranges"][request["range"]]
        data = bytes.fromhex(item["hex"])
        offset = request["byte_offset"]
        if offset < 0 or offset + 8 > len(data) or offset % 8:
            raise ValueError("Requested pointer word bound")
        words.append({**request, "pointer_address": item["value"] + offset,
                      "raw": int.from_bytes(data[offset:offset + 8], "little")})
    for item in result.get("event_stubs", {}).values():
        words.append({"stub_address": item["address"], "pointer_address": item["pointer_slot"],
                      "raw": int.from_bytes(bytes.fromhex(item["pointer_hex"]), "little")})
    if len(words) > 128:
        raise ValueError("Exact event target count bound")
    targets = {0: set(), 1: set()}
    targets[1].update(item["address"] for item in result.get("event_direct_calls", {}).values())
    for word in words:
        raw = word["raw"]
        level = (raw >> 30) & 3
        if raw >> 63 or (raw >> 32) & ((1 << 19) - 1) or level > 1:
            raise ValueError("Unexpected event pointer encoding")
        target = raw & ((1 << 30) - 1)
        targets[level].add(target + (KERNEL_CACHE_BASE if level == 0 else 0))
        word.update(cache_level=level, target_offset=target)
    result["event_target_words"] = words
    result["event_target_symbols"] = []
    total_names = 0
    for collection, expected in EVENT_SYMBOL_SOURCES.items():
        path = Path(collection)
        with path.open("rb") as file:
            size = path.stat().st_size
            info, commands = header(file, 0, size)
            if info["header_sha256"] != expected["header_sha256"] or image_uuid(commands) != expected["uuid"]:
                raise ValueError("Event symbol source collection changed")
            for entry in expected["entries"]:
                info, commands = header(file, entry["file_offset"], size)
                if info["header_sha256"] != entry["header_sha256"] or image_uuid(commands) != entry["uuid"]:
                    raise ValueError("Event symbol source image changed")
                sym = next(data for kind, data in commands if kind == 2)
                symoff, count, stroff, strbytes = struct.unpack_from("<IIII", sym, 8)
                if count > 32768 or stroff + strbytes > size:
                    raise ValueError("Event symbol source bounds")
                data = read_at(file, symoff, count * 16, size)
                if hashlib.sha256(data).hexdigest() != entry["symbol_data_sha256"]:
                    raise ValueError("Event source nlists changed")
                level = expected["cache_level"]
                for i in range(count):
                    strx, kind, section, descriptor, value = struct.unpack_from("<IBBHQ", data, i * 16)
                    if value not in targets[level] or not section or kind & 0xE0 or kind & 0x0E != 0x0E:
                        continue
                    if strx >= strbytes:
                        raise ValueError("Exact event name index bound")
                    name = read_at(file, stroff + strx, min(512, strbytes - strx), size)
                    total_names += len(name)
                    if total_names > 65536 or b"\0" not in name:
                        raise ValueError("Exact event symbol name read bound")
                    result["event_target_symbols"].append({"name": name.split(b"\0", 1)[0].decode(),
                        "cache_level": level, "value": value,
                        "target_offset": value - (KERNEL_CACHE_BASE if level == 0 else 0),
                        "entry": entry["identifier"]})
    result["event_name_bytes_read"] = total_names
