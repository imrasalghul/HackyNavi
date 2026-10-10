#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read only two Metal method implementations and bounded image metadata.

No Objective-C message send, object allocation, IOKit client open or GPU work.
Code is data here. Execute via python3 stdin; output JSON; no remote files.
"""
import ctypes as c
import hashlib
import json
import platform
import struct
import subprocess
import uuid

if platform.system() != "Darwin" or platform.machine() != "x86_64":
    raise SystemExit("Requires the actual Intel macOS runtime")

metal = c.CDLL("/System/Library/Frameworks/Metal.framework/Metal")
ioaccel = c.CDLL("/System/Library/PrivateFrameworks/IOAccelerator.framework/IOAccelerator")
objc = c.CDLL("/usr/lib/libobjc.A.dylib")
system = c.CDLL(None)


class DlInfo(c.Structure):
    _fields_ = [("name", c.c_char_p), ("base", c.c_void_p),
                ("symbol", c.c_char_p), ("symbol_address", c.c_void_p)]


system.dladdr.argtypes, system.dladdr.restype = [c.c_void_p, c.POINTER(DlInfo)], c.c_int
for name, args, result in (
    ("objc_getClass", [c.c_char_p], c.c_void_p),
    ("sel_registerName", [c.c_char_p], c.c_void_p),
    ("class_getInstanceMethod", [c.c_void_p, c.c_void_p], c.c_void_p),
    ("method_getImplementation", [c.c_void_p], c.c_void_p),
    ("method_getTypeEncoding", [c.c_void_p], c.c_char_p),
):
    function = getattr(objc, name)
    function.argtypes, function.restype = args, result


def location(address):
    result = DlInfo()
    if not system.dladdr(address, c.byref(result)):
        return {"resolved": False}
    return {"resolved": True, "image": result.name.decode(),
            "base": result.base, "image_offset": address - result.base,
            "nearest_symbol": result.symbol.decode() if result.symbol else None,
            "symbol_offset": address - result.symbol_address if result.symbol_address else None}


def image_layout(base):
    header = c.string_at(base, 32)
    magic, cpu, subtype, filetype, count, length, flags, reserved = struct.unpack("<IiiIIIII", header)
    if magic != 0xFEEDFACF or cpu != 0x1000007 or count > 512 or length > 1024 * 1024:
        raise RuntimeError("Unexpected Mach-O header")
    commands = c.string_at(base + 32, length)
    offset, segments, sections, image_uuid, starts = 0, [], [], None, None
    for _ in range(count):
        if offset + 8 > length:
            raise RuntimeError("Load-command header exceeds declared bytes")
        kind, size = struct.unpack_from("<II", commands, offset)
        if size < 8 or offset + size > length:
            raise RuntimeError("Invalid load-command size")
        data = commands[offset:offset + size]
        if kind == 0x19:
            if size < 72:
                raise RuntimeError("Invalid 64-bit segment")
            name, va, vmbytes, fileoff, filebytes, maxprot, initprot, nsects, segflags = struct.unpack_from("<16sQQQQiiII", data, 8)
            name = name.rstrip(b"\0").decode()
            if nsects > 256 or 72 + nsects * 80 > size:
                raise RuntimeError("Section count exceeds segment")
            segments.append({"name": name, "va": va, "bytes": vmbytes,
                             "fileoff": fileoff, "filebytes": filebytes, "initprot": initprot})
            for i in range(nsects):
                section, segment, sva, sbytes = struct.unpack_from("<16s16sQQ", data, 72 + i * 80)
                sections.append({"name": section.rstrip(b"\0").decode(), "segment": segment.rstrip(b"\0").decode(),
                                 "va": sva, "bytes": sbytes})
        elif kind == 0x1B:
            if size != 24:
                raise RuntimeError("Unexpected UUID command")
            image_uuid = str(uuid.UUID(bytes=data[8:24]))
        elif kind == 0x26:
            if size != 16:
                raise RuntimeError("Unexpected function-starts command")
            starts = struct.unpack_from("<II", data, 8)
        offset += size
    if offset != length or not image_uuid:
        raise RuntimeError("Incomplete image load commands or UUID")
    text = next(s for s in segments if s["name"] == "__TEXT")
    slide = base - text["va"]
    for s in segments + sections:
        s["address"] = slide + s["va"]
    functions, starts_note = [], "LC_FUNCTION_STARTS absent"
    if starts:
        linkedit = next((s for s in segments if s["name"] == "__LINKEDIT"), None)
        dataoff, databytes = starts
        if not linkedit or databytes > 256 * 1024:
            starts_note = "Function-starts data unavailable within bounds"
        else:
            address = linkedit["address"] + dataoff - linkedit["fileoff"]
            if address < linkedit["address"] or address + databytes > linkedit["address"] + linkedit["bytes"]:
                starts_note = "Function-starts data outside mapped LINKEDIT"
            else:
                encoded = c.string_at(address, databytes)
                value = shift = 0
                current = base
                terminated = False
                for byte in encoded:
                    if shift >= 64 or (shift == 63 and (byte & 127) > 1):
                        raise RuntimeError("Function-start delta exceeds uint64")
                    value |= (byte & 127) << shift
                    if byte & 128:
                        shift += 7
                        continue
                    if not value:
                        terminated = True
                        break
                    current += value
                    if current >= 1 << 64:
                        raise RuntimeError("Function-start address exceeds uint64")
                    functions.append(current)
                    value = shift = 0
                    if len(functions) > 65536:
                        raise RuntimeError("Too many function starts")
                starts_note = "decoded" if terminated else "unterminated"
                if not terminated:
                    functions = []
    return {"uuid": image_uuid, "header_sha256": hashlib.sha256(header + commands).hexdigest(),
            "base": base, "segments": segments, "sections": sections,
            "functions": functions, "function_starts_note": starts_note}


report = {"system": subprocess.check_output(["sw_vers"], text=True, timeout=10).strip(),
          "methods": {}, "images": {}, "named_exports": {},
          "limits": {"methods": 2, "bytes_per_method": 8192, "load_commands_bytes": 1048576,
                     "function_starts_bytes": 262144},
          "device_enumerated": False, "device_instantiated": False, "user_client_opened": False,
          "objc_messages_sent": False, "gpu_submitted": False, "remote_files_created": False}
layouts = {}
cls = objc.objc_getClass(b"MTLIOAccelDevice")
if not cls:
    raise RuntimeError("Actual MTLIOAccelDevice class missing")
for selector in ("initWithAcceleratorPort:", "lazyInitialize"):
    method = objc.class_getInstanceMethod(cls, objc.sel_registerName(selector.encode()))
    if not method:
        raise RuntimeError("Expected method missing: " + selector)
    implementation = objc.method_getImplementation(method)
    loc = location(implementation)
    if not loc["resolved"] or "/Metal.framework/" not in loc["image"]:
        raise RuntimeError("Unexpected implementation image")
    base = loc["base"]
    if base not in layouts:
        layouts[base] = image_layout(base)
    layout = layouts[base]
    section = next((s for s in layout["sections"] if s["segment"] == "__TEXT" and s["name"] == "__text"
                    and s["address"] <= implementation < s["address"] + s["bytes"]), None)
    if not section:
        raise RuntimeError("Implementation is outside named executable text section")
    exact_start = implementation in layout["functions"]
    end = section["address"] + section["bytes"]
    following = next((f for f in layout["functions"] if f > implementation), None)
    if exact_start and following:
        end = min(end, following)
    count = min(end - implementation, 8192)
    data = c.string_at(implementation, count)
    item = dict(loc, encoding=objc.method_getTypeEncoding(method).decode(), implementation=implementation,
                image_uuid=layout["uuid"], bytes=len(data), sha256=hashlib.sha256(data).hexdigest(),
                function_start_matched=exact_start, function_end_bounded=bool(exact_start and following),
                truncated=count < end - implementation, code_hex=data.hex())
    report["methods"][selector] = item
for base, layout in layouts.items():
    report["images"][str(base)] = {key: value for key, value in layout.items() if key != "functions"}
    report["images"][str(base)]["function_start_count"] = len(layout["functions"])

for name in ("IOAccelDeviceCreate", "IOAccelDeviceRelease", "IOAccelDeviceGetConfig", "IOAccelDeviceGetName",
             "IOAccelSharedCreate", "IOAccelSharedRelease", "IOAccelContextCreate", "IOServiceOpen",
             "IOServiceClose", "IOConnectCallMethod", "IOConnectCallStructMethod", "IOConnectMapMemory64",
             "IOConnectAddClient", "objc_msgSend", "objc_msgSendSuper2", "dispatch_release"):
    for lib in (system, metal, ioaccel, objc):
        try:
            function = getattr(lib, name)
            address = c.cast(function, c.c_void_p).value
            report["named_exports"][name] = dict(location(address), address=address)
            break
        except AttributeError:
            continue
    else:
        report["named_exports"][name] = {"resolved": False}

print(json.dumps(report, indent=2, sort_keys=True))
