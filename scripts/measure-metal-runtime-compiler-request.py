#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Resolve measured source-builder callbacks and its two exact lower callees.

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

# The following addresses come only from the retained exact source-library
# method bodies, not arbitrary framework scanning. See local freeze provenance.
EXPECTED_METAL_UUID = "c3efbabe-6b06-33ab-bc45-43fe86297aa1"
MAX_TOTAL_CODE, MAX_OUTPUT = 16384, 131072
report = {'system':subprocess.check_output(['sw_vers'],text=True,timeout=10).strip(),
          'helpers':{},'explicit_references':{},'ivars':{},'images':{},
          'device_enumerated':False,'device_instantiated':False,'objc_messages_sent':False,
          'compiler_invoked':False,'user_client_opened':False,'gpu_submitted':False,
          'remote_files_created':False}
cls=objc.objc_getClass(b'_MTLDevice')
method=objc.class_getInstanceMethod(cls,objc.sel_registerName(b'newLibraryWithSource:options:error:'))
base=location(objc.method_getImplementation(method))['base']
layout=image_layout(base)
if layout['uuid']!=EXPECTED_METAL_UUID:raise RuntimeError('Metal image changed')
layouts={base:layout};total=0

def capture(name,offset,limit=1024):
    global total
    address=base+offset;loc=location(address)
    if not loc['resolved'] or loc['base']!=base:raise RuntimeError('Selected helper moved image')
    item=dict(loc,image_uuid=layout['uuid']);report['helpers'][name]=item
    section=next((s for s in layout['sections'] if s['name']=='__text' and s['segment']=='__TEXT' and s['address']<=address<s['address']+s['bytes']),None)
    following=next((f for f in layout['functions'] if f>address),None)
    if not section or address not in layout['functions'] or following is None:
        item['skipped']='Exact function bounds unavailable';return
    count=min(following,section['address']+section['bytes'])-address
    item['declared_function_bytes']=count
    item['code_limit_bytes']=limit
    if count<=0 or count>limit or total+count>MAX_TOTAL_CODE:
        item['skipped']='Explicit code bound';return
    raw=c.string_at(address,count);total+=count
    item.update(bytes=count,code_hex=raw.hex(),sha256=hashlib.sha256(raw).hexdigest(),
                function_start_matched=True,function_end_bounded=True,truncated=False)

# Hash the already captured complete bodies without publishing them again.
# The only new code copied below is the two observed callees and two exact
# device query implementations. No framework method is invoked.
SOURCES = {'source_library_builder': {'offset': 1379888, 'bytes': 8001, 'sha256': 'f9ceee83d88d2337ba2673baa9b548e45aff301e4aa366a5255075017265e2ed'}, '_MTLLibrary::initWithLibraryContainer:device:': {'offset': 127612, 'bytes': 164, 'sha256': 'd42940cab45d17c1793850b7c2cc581f00857afacc3880e49fdef20fb1e1b980'}, 'synchronous_source_completion': {'offset': 126569, 'bytes': 147, 'sha256': '5ee98818d89e9314ef97d1a3a16025b9040453ea57cd2e84f56af71ec21cfa7c'}, '_MTLLibrary::dealloc': {'offset': 168596, 'bytes': 166, 'sha256': '8b095e8a3f5bc9b4f702471b982344e9ffa5616fd3a04b8bb864ac6693ae4433'}}
REQUESTS = [{'source': 'source_library_builder', 'instruction_image_offset': 1380057, 'instruction_hex': '488b3500aac833', 'target_image_offset': 870168800}, {'source': 'source_library_builder', 'instruction_image_offset': 1380079, 'instruction_hex': '488b35faa9c833', 'target_image_offset': 870168816}, {'source': 'source_library_builder', 'instruction_image_offset': 1380099, 'instruction_hex': '488b358e7bc833', 'target_image_offset': 870156952}, {'source': 'source_library_builder', 'instruction_image_offset': 1380121, 'instruction_hex': '488b1de0a9c833', 'target_image_offset': 870168832}, {'source': 'source_library_builder', 'instruction_image_offset': 1380251, 'instruction_hex': '488b35e6a8c833', 'target_image_offset': 870168712}, {'source': 'source_library_builder', 'instruction_image_offset': 1380412, 'instruction_hex': '488b354da8c833', 'target_image_offset': 870168720}, {'source': 'source_library_builder', 'instruction_image_offset': 1380430, 'instruction_hex': '488b35bbaac833', 'target_image_offset': 870169360}, {'source': 'source_library_builder', 'instruction_image_offset': 1380515, 'instruction_hex': '488b35eea7c833', 'target_image_offset': 870168728}, {'source': 'source_library_builder', 'instruction_image_offset': 1380538, 'instruction_hex': '488b35dfa7c833', 'target_image_offset': 870168736}, {'source': 'source_library_builder', 'instruction_image_offset': 1380628, 'instruction_hex': '488b35a5a7c833', 'target_image_offset': 870168768}, {'source': 'source_library_builder', 'instruction_image_offset': 1380647, 'instruction_hex': '488b35daa7c833', 'target_image_offset': 870168840}, {'source': 'source_library_builder', 'instruction_image_offset': 1380677, 'instruction_hex': '488b35c4a7c833', 'target_image_offset': 870168848}, {'source': 'source_library_builder', 'instruction_image_offset': 1380700, 'instruction_hex': '488b35ad7cc833', 'target_image_offset': 870157840}, {'source': 'source_library_builder', 'instruction_image_offset': 1380719, 'instruction_hex': '488b3542a7c833', 'target_image_offset': 870168760}, {'source': 'source_library_builder', 'instruction_image_offset': 1380814, 'instruction_hex': '488b35f3a6c833', 'target_image_offset': 870168776}, {'source': 'source_library_builder', 'instruction_image_offset': 1380851, 'instruction_hex': '488b35b6a6c833', 'target_image_offset': 870168752}, {'source': 'source_library_builder', 'instruction_image_offset': 1380914, 'instruction_hex': '488b1dafa6c833', 'target_image_offset': 870168808}, {'source': 'source_library_builder', 'instruction_image_offset': 1380996, 'instruction_hex': '488b359d6ec833', 'target_image_offset': 870154536}, {'source': 'source_library_builder', 'instruction_image_offset': 1381086, 'instruction_hex': '488b05c38ac833', 'target_image_offset': 870161832}, {'source': 'source_library_builder', 'instruction_image_offset': 1381100, 'instruction_hex': '488b0555a4c833', 'target_image_offset': 870168392}, {'source': 'source_library_builder', 'instruction_image_offset': 1381128, 'instruction_hex': '488b05296ec833', 'target_image_offset': 870154552}, {'source': 'source_library_builder', 'instruction_image_offset': 1382291, 'instruction_hex': '488b355ea1c833', 'target_image_offset': 870168824}, {'source': 'source_library_builder', 'instruction_image_offset': 1382312, 'instruction_hex': '488b1d69a3c833', 'target_image_offset': 870169368}, {'source': 'source_library_builder', 'instruction_image_offset': 1382425, 'instruction_hex': '488b35f8a0c833', 'target_image_offset': 870168856}, {'source': 'source_library_builder', 'instruction_image_offset': 1382476, 'instruction_hex': '488b3585a0c833', 'target_image_offset': 870168792}, {'source': 'source_library_builder', 'instruction_image_offset': 1382544, 'instruction_hex': '488b3511a0c833', 'target_image_offset': 870168744}, {'source': 'source_library_builder', 'instruction_image_offset': 1382876, 'instruction_hex': '488b058570c833', 'target_image_offset': 870156904}, {'source': 'source_library_builder', 'instruction_image_offset': 1382890, 'instruction_hex': '4c8b355f67c833', 'target_image_offset': 870154576}, {'source': 'source_library_builder', 'instruction_image_offset': 1382897, 'instruction_hex': '488b056067c833', 'target_image_offset': 870154584}, {'source': 'source_library_builder', 'instruction_image_offset': 1383809, 'instruction_hex': '488b35489bc833', 'target_image_offset': 870168784}, {'source': 'source_library_builder', 'instruction_image_offset': 1383967, 'instruction_hex': '488b35da80c833', 'target_image_offset': 870162176}, {'source': 'source_library_builder', 'instruction_image_offset': 1383988, 'instruction_hex': '488b352d62c833', 'target_image_offset': 870154344}, {'source': 'source_library_builder', 'instruction_image_offset': 1384357, 'instruction_hex': '488b35e49ac833', 'target_image_offset': 870169232}, {'source': 'source_library_builder', 'instruction_image_offset': 1384846, 'instruction_hex': '488b051b5ec833', 'target_image_offset': 870154160}, {'source': 'source_library_builder', 'instruction_image_offset': 1387048, 'instruction_hex': '488b353156c833', 'target_image_offset': 870154336}, {'source': 'source_library_builder', 'instruction_image_offset': 1387122, 'instruction_hex': '488b353f56c833', 'target_image_offset': 870154424}, {'source': 'source_library_builder', 'instruction_image_offset': 1387150, 'instruction_hex': '488b352b56c833', 'target_image_offset': 870154432}, {'source': 'source_library_builder', 'instruction_image_offset': 1387238, 'instruction_hex': '4c8b3d3390c833', 'target_image_offset': 870169376}, {'source': 'synchronous_source_completion', 'instruction_image_offset': 126585, 'instruction_hex': '4c8b3d089ece32', 'target_image_offset': 852528264}, {'source': 'synchronous_source_completion', 'instruction_image_offset': 126632, 'instruction_hex': '488b3551b8db33', 'target_image_offset': 870164224}, {'source': '_MTLLibrary::initWithLibraryContainer:device:', 'instruction_image_offset': 127642, 'instruction_hex': '488b0df7e3db33', 'target_image_offset': 870176408}, {'source': '_MTLLibrary::initWithLibraryContainer:device:', 'instruction_image_offset': 127653, 'instruction_hex': '488b35248ddb33', 'target_image_offset': 870154192}, {'source': '_MTLLibrary::initWithLibraryContainer:device:', 'instruction_image_offset': 127698, 'instruction_hex': '488b0defc3aa36', 'target_image_offset': 917288648}, {'source': '_MTLLibrary::initWithLibraryContainer:device:', 'instruction_image_offset': 127709, 'instruction_hex': '488b05ecc3aa36', 'target_image_offset': 917288656}, {'source': '_MTLLibrary::initWithLibraryContainer:device:', 'instruction_image_offset': 127729, 'instruction_hex': '488b05c8c3aa36', 'target_image_offset': 917288640}, {'source': '_MTLLibrary::initWithLibraryContainer:device:', 'instruction_image_offset': 127747, 'instruction_hex': '488b0dcec3aa36', 'target_image_offset': 917288664}, {'source': '_MTLLibrary::dealloc', 'instruction_image_offset': 168692, 'instruction_hex': '488b352d28db33', 'target_image_offset': 870169384}, {'source': '_MTLLibrary::dealloc', 'instruction_image_offset': 168741, 'instruction_hex': '488b35acecda33', 'target_image_offset': 870154200}, {'source': 'source_library_builder', 'instruction_image_offset': 1379972, 'instruction_hex': 'e8f9bc0700', 'target_image_offset': 1887106}, {'source': 'source_library_builder', 'instruction_image_offset': 1379983, 'instruction_hex': 'e8f4bc0700', 'target_image_offset': 1887112}, {'source': 'source_library_builder', 'instruction_image_offset': 1380791, 'instruction_hex': 'e8f5f3ecff', 'target_image_offset': 132529}, {'source': 'source_library_builder', 'instruction_image_offset': 1382593, 'instruction_hex': 'e85c09ffff', 'target_image_offset': 1319458}, {'source': 'source_library_builder', 'instruction_image_offset': 1383902, 'instruction_hex': 'e87452faff', 'target_image_offset': 1011799}, {'source': 'source_library_builder', 'instruction_image_offset': 1386594, 'instruction_hex': 'e871a10700', 'target_image_offset': 1886680}, {'source': 'source_library_builder', 'instruction_image_offset': 1386846, 'instruction_hex': 'e853e2ffff', 'target_image_offset': 1379254}, {'source': 'source_library_builder', 'instruction_image_offset': 1387255, 'instruction_hex': 'e898a00700', 'target_image_offset': 1887124}]

for name, expected in SOURCES.items():
    address=base+expected['offset']
    next_start=next((f for f in layout['functions'] if f>address),None)
    section=next((s for s in layout['sections'] if s['name']=='__text' and s['segment']=='__TEXT' and s['address']<=address and address+expected['bytes']<=s['address']+s['bytes']),None)
    if not section or address not in layout['functions'] or next_start!=address+expected['bytes']:
        raise RuntimeError('Prior complete function extent changed')
    raw=c.string_at(address,expected['bytes'])
    if hashlib.sha256(raw).hexdigest()!=expected['sha256']:
        raise RuntimeError('Prior measured source bytes changed')
    for item in REQUESTS:
        if item['source']!=name:continue
        begin=item['instruction_image_offset']-expected['offset']
        if raw[begin:begin+len(bytes.fromhex(item['instruction_hex']))]!=bytes.fromhex(item['instruction_hex']):
            raise RuntimeError('Reference instruction differs')
report['prior_sources_verified']=SOURCES
for name,offset in [('source_compiler_version_selection',0x142222),('source_library_request',0x150bb6)]:
    capture(name,offset,1024)
objc.sel_getName.argtypes, objc.sel_getName.restype = [c.c_void_p], c.c_void_p
objc.class_getName.argtypes, objc.class_getName.restype = [c.c_void_p], c.c_void_p
system.mach_vm_region.argtypes = [c.c_uint, c.POINTER(c.c_uint64), c.POINTER(c.c_uint64),
                                 c.c_int, c.POINTER(c.c_int), c.POINTER(c.c_uint), c.POINTER(c.c_uint)]
system.mach_vm_region.restype = c.c_int
system.mach_port_deallocate.argtypes, system.mach_port_deallocate.restype = [c.c_uint, c.c_uint], c.c_int
own_task = c.c_uint.in_dll(system, "mach_task_self_").value


def bounded_pointer_region(address):
    # Public mach_vm_region ABI; VM_REGION_BASIC_INFO_64 is 9 and its
    # pack(4) 36-byte structure has 9 integer words (mach/vm_region.h).
    start, size, count, object_name = c.c_uint64(address), c.c_uint64(), c.c_uint(9), c.c_uint()
    info = (c.c_int * 9)()
    kr = system.mach_vm_region(own_task, c.byref(start), c.byref(size), 9,
                               info, c.byref(count), c.byref(object_name))
    released = None
    if object_name.value:
        released = system.mach_port_deallocate(own_task, object_name.value)
    result = {"query_status": kr, "object_name_release_status": released,
              "address": start.value, "bytes": size.value, "info_words": count.value}
    readable = (kr == 0 and count.value == 9 and info[0] & 1
                and start.value <= address and address + 8 <= start.value + size.value)
    if kr == 0:
        result["protection"] = info[0]
    if readable:
        result["word"] = struct.unpack("<Q", c.string_at(address, 8))[0]
    return result


def bounded_cstring(pointer, maximum=192):
    # Only known runtime-returned names enter this function; stop at the NUL,
    # so a valid name ending at a page boundary never reads the next page.
    if not pointer:
        return {"text": None, "terminated": False}
    result = bytearray()
    for i in range(maximum):
        value = c.string_at(pointer + i, 1)
        if value == b"\0":
            return {"text": result.decode("utf-8", errors="replace"), "terminated": True}
        result.extend(value)
    return {"text": result.decode("utf-8", errors="replace"), "terminated": False}


def described(address):
    loc = location(address)
    if not loc["resolved"]:
        return loc, None, None
    base = loc["base"]
    if base not in layouts:
        if len(layouts) >= 16:
            return dict(loc, skipped="image count bound"), None, None
        layouts[base] = image_layout(base)
    layout = layouts[base]
    loc["image_uuid"] = layout["uuid"]
    section = next((s for s in layout["sections"] if s["address"] <= address < s["address"] + s["bytes"]), None)
    segment = next((s for s in layout["segments"] if s["address"] <= address < s["address"] + s["bytes"]), None)
    if section:
        loc["section"] = section["name"]
        loc["segment"] = section["segment"]
    return loc, section, segment


def reference(address, allow_stub=True):
    loc, section, segment = described(address)
    result = {"address": address, "location": loc}
    if not section or not segment or not (segment["initprot"] & 1):
        # The dyld shared cache coalesces some GOT slots outside individual
        # images. Read only this explicit code-derived 8-byte location after
        # checking this process's actual readable VM region; never enumerate.
        region = bounded_pointer_region(address)
        result["vm_region"] = region
        if "word" in region:
            result["word"] = region["word"]
            if region["word"]:
                result["pointee_location"] = location(region["word"])
        else:
            result["skipped"] = "No verified readable 8-byte region"
        return result
    remaining = section["address"] + section["bytes"] - address
    if section["name"] == "__stubs" and allow_stub:
        data = c.string_at(address, min(remaining, 16))
        result["stub_hex"] = data.hex()
        if data[:2] == b"\xff\x25" and len(data) >= 6:
            target = address + 6 + int.from_bytes(data[2:6], "little", signed=True)
            result["stub_pointer"] = reference(target, False)
        return result
    if section["name"] in ("__cstring", "__objc_methname", "__objc_classname"):
        data = c.string_at(address, min(remaining, 192))
        value, separator, ignored = data.partition(b"\0")
        result["string"] = {"text": value.decode("utf-8", errors="replace"), "terminated": bool(separator)}
        return result
    pointer_sections = {"__got", "__auth_got", "__objc_ivar", "__objc_selrefs", "__objc_classrefs",
                        "__objc_superrefs", "__nl_symbol_ptr", "__la_symbol_ptr", "__const"}
    if section["name"] not in pointer_sections or remaining < 8:
        result["skipped"] = "Reference is not a selected pointer/string section"
        return result
    pointer = struct.unpack("<Q", c.string_at(address, 8))[0]
    result["word"] = pointer
    if section["name"] == "__objc_ivar":
        result["kind"] = "ivar offset value; not dereferenced"
    elif pointer:
        result["pointee_location"] = location(pointer)
        if section["name"] == "__objc_selrefs":
            result["selector"] = bounded_cstring(objc.sel_getName(pointer))
        elif section["name"] in ("__objc_classrefs", "__objc_superrefs"):
            result["class_name"] = bounded_cstring(objc.class_getName(pointer))
    return result


report['explicit_references']={str(item['target_image_offset']):reference(base+item['target_image_offset']) for item in REQUESTS}
report['device_queries']={}
for offset in (0x33ddbb10,0x33ddbb18):
    item=report['explicit_references'][str(offset)]
    selector=item.get('selector',{})
    if not selector.get('terminated') or not selector.get('text'):
        raise RuntimeError('Exact device selector reference not resolved')
    text=selector['text'];cls=objc.objc_getClass(b'_MTLDevice')
    method=objc.class_getInstanceMethod(cls,objc.sel_registerName(text.encode()))
    if not method:report['device_queries'][text]={'present':False};continue
    address=objc.method_getImplementation(method)
    report['device_queries'][text]={'present':True,'encoding':objc.method_getTypeEncoding(method).decode(),'location':location(address)}
    if location(address).get('base')==base:capture('_MTLDevice::'+text,address-base,1024)
report['total_new_code_bytes']=total
report['limits']={'total_code_bytes':MAX_TOTAL_CODE,'output_bytes':MAX_OUTPUT,'new_function_code_bytes':1024,'references':len(REQUESTS),'reference_pointer_bytes':8,'reference_stub_bytes':16,'reference_string_bytes':192}
report['images']={str(b):{k:v[k] for k in ('uuid','header_sha256','base','function_starts_note')} for b,v in layouts.items()}
output=json.dumps(report,indent=2,sort_keys=True)
if len(output.encode())+1>MAX_OUTPUT:raise RuntimeError('Output exceeds128KiB bound')
print(output)
