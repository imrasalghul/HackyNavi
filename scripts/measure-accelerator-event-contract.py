# SPDX-License-Identifier: MIT
"""Bounded read-only event/stamp code ranges from identified KC symbols.

Use after the common collection metadata-reader helpers. REQUESTS and
EXPECTED_SYSTEM are pinned to earlier evidence. No GPU/runtime object calls.
"""
import os
import selectors
import signal
import time


def bounded_command(argv, output_limit=131072, seconds=15):
    child = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, start_new_session=True)
    selector = selectors.DefaultSelector()
    selector.register(child.stdout, selectors.EVENT_READ, "stdout")
    selector.register(child.stderr, selectors.EVENT_READ, "stderr")
    streams = {"stdout": bytearray(), "stderr": bytearray()}
    deadline = time.monotonic() + seconds
    reason = None
    while selector.get_map():
        if time.monotonic() >= deadline:
            reason = "time bound"
            break
        for key, _ in selector.select(timeout=0.2):
            data = os.read(key.fileobj.fileno(), 4096)
            if not data:
                selector.unregister(key.fileobj)
                continue
            streams[key.data].extend(data)
            if sum(map(len, streams.values())) > output_limit:
                reason = "output bound"
                break
        if reason:
            break
    if reason:
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    code = child.wait(timeout=3)
    selector.close()
    return {"argv": argv, "exit_code": code, "stopped_reason": reason,
            **{key: bytes(value[:output_limit]).decode(errors="replace") for key, value in streams.items()}}


if not 0 < len(REQUESTS) <= 12:
    raise ValueError("Request count bound")
path = Path("/System/Library/KernelCollections/SystemKernelExtensions.kc")
result = {"ranges": {}, "entries": {}, "remote_files_created": False, "running_kernel_read": False,
          "device_instantiated": False, "gpu_submitted": False, "kernel_loaded": False,
          "symbol_range_note": "A next named symbol bounds bytes, not necessarily a function end; code may contain unnamed sequences or be explicitly truncated."}
with path.open("rb") as file:
    size = path.stat().st_size
    root, commands = header(file, 0, size)
    uid = next(str(uuid.UUID(bytes=data[8:24])) for kind, data in commands if kind == 0x1B)
    if root["header_sha256"] != EXPECTED_SYSTEM["header_sha256"] or uid != EXPECTED_SYSTEM["uuid"]:
        raise ValueError("Collection changed")
    result["collection"] = {**root, "uuid": uid, "path": str(path)}
    total = 0
    for pinned in EXPECTED_SYSTEM["entries"]:
        relevant = [r for r in REQUESTS if r["entry"] == pinned["identifier"]]
        if not relevant:
            continue
        info, entry_commands = header(file, pinned["file_offset"], size)
        uid = next(str(uuid.UUID(bytes=data[8:24])) for kind, data in entry_commands if kind == 0x1B)
        if info["header_sha256"] != pinned["header_sha256"] or uid != pinned["uuid"]:
            raise ValueError("Selected entry changed")
        segments = []
        for kind, data in entry_commands:
            if kind == 0x19:
                name, va, vmbytes, fileoff, filebytes, maxprot, initprot, nsects, flags = struct.unpack_from("<16sQQQQiiII", data, 8)
                segments.append({"name": name.rstrip(b"\0").decode(), "vmaddr": va,
                                 "file_offset": fileoff, "file_bytes": filebytes, "protection": initprot})
        sym = next(data for kind, data in entry_commands if kind == 2)
        symoff, count, stroff, strbytes = struct.unpack_from("<IIII", sym, 8)
        if count > 8192:
            raise ValueError("Selected entry nlist count bound")
        nlists = read_at(file, symoff, count * 16, size)
        if hashlib.sha256(nlists).hexdigest() != pinned["symbol_data_sha256"]:
            raise ValueError("Selected entry nlists changed")
        defined = [struct.unpack_from("<IBBHQ", nlists, i * 16) for i in range(count)]
        defined = [s for s in defined if not s[1] & 0xE0 and s[1] & 0x0E == 0x0E and s[2] and s[4]]
        result["entries"][pinned["identifier"]] = {**info, "uuid": uid, "segments": segments}
        for request in relevant:
            candidates = [s for s in defined if s[4] == request["value"]]
            names = []
            for s in candidates:
                if s[0] >= strbytes:
                    raise ValueError("Symbol name index bound")
                data = read_at(file, stroff + s[0], min(512, strbytes - s[0]), size)
                if b"\0" not in data:
                    raise ValueError("Selected symbol name length bound")
                names.append(data.split(b"\0", 1)[0].decode())
            if request.get("symbol") and request["symbol"] not in names:
                raise ValueError("Selected symbol identity changed")
            if not names:
                raise ValueError("Observed reference does not name a defined symbol")
            end = min(s[4] for s in defined if s[4] > request["value"])
            maximum = 2048 if request["kind"] == "code" else (4096 if request["kind"] == "table" else 64)
            length = min(end - request["value"], maximum)
            segment = next((s for s in segments if s["vmaddr"] <= request["value"] and
                            request["value"] + length <= s["vmaddr"] + s["file_bytes"]), None)
            if not segment or (request["kind"] == "code" and not segment["protection"] & 4):
                raise ValueError("Requested range lacks its required file-backed segment")
            total += length
            if total > 16384:
                raise ValueError("Combined range byte bound")
            offset = segment["file_offset"] + request["value"] - segment["vmaddr"]
            data = read_at(file, offset, length, size)
            result["ranges"][request.get("symbol") or request["label"]] = {**request, "actual_symbol_names": names, "file_offset": offset, "bytes": length,
                "next_symbol_address": end, "truncated": length != end - request["value"],
                "hex": data.hex(), "sha256": hashlib.sha256(data).hexdigest(), "segment": segment["name"]}
    if len(result["ranges"]) != len(REQUESTS):
        raise ValueError("Not all requested ranges were captured")
if globals().get("CAPTURE_FEASIBILITY", True):
    result["storage"] = bounded_command(["df", "-Pk", os.path.expanduser("~")], 4096)
    help_result = bounded_command(["xcodebuild", "-help"])
    for key in ("stdout", "stderr"):
        original = help_result[key]
        lines = original.splitlines()
        selected = set()
        for i, line in enumerate(lines):
            if any(word in line for word in ("downloadComponent", "importComponent", "exportPath")):
                selected.update(range(i, min(i + 3, len(lines))))
        help_result[key + "_full_bytes"] = len(original.encode())
        help_result[key + "_full_sha256"] = hashlib.sha256(original.encode()).hexdigest()
        help_result[key] = "\n".join(lines[i] for i in sorted(selected))[:8192]
    result["component_options"] = help_result
result["limits"] = {"requested_ranges": 12, "code_bytes_each": 2048, "table_bytes_each": 4096,
                    "data_bytes_each": 64, "range_bytes_total": 16384,
                    "nlist_count_per_entry": 8192, "component_help_output_bytes": 131072}
print(json.dumps(result, indent=2, sort_keys=True))
