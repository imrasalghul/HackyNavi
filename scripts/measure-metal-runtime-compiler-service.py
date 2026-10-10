#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read the exact runtime source compiler cache-miss and result contract.

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
SOURCES = {'source_library_request': {'offset': 1379254, 'bytes': 219, 'sha256': 'b52f8a1c412b19449aa94960248cf4e19b01399c034c8b6f164a72f02454ebd9'}, 'source_compiler_version_selection': {'offset': 1319458, 'bytes': 262, 'sha256': '7d2522d68e19689d5964e13742ed68f639f0513b6607be35f631653da6f3d054'}}
REQUESTS = [{'source': 'source_compiler_version_selection', 'instruction_image_offset': 1319470, 'instruction_hex': 'e85e9afbff', 'target_image_offset': 1031313}, {'source': 'source_compiler_version_selection', 'instruction_image_offset': 1319479, 'instruction_hex': '48833d49489936ff', 'target_image_offset': 917334664}, {'source': 'source_compiler_version_selection', 'instruction_image_offset': 1319493, 'instruction_hex': '8b0535489936', 'target_image_offset': 917334656}, {'source': 'source_compiler_version_selection', 'instruction_image_offset': 1319516, 'instruction_hex': '48833d94479936ff', 'target_image_offset': 917334520}, {'source': 'source_compiler_version_selection', 'instruction_image_offset': 1319530, 'instruction_hex': '803d7f47993601', 'target_image_offset': 917334512}, {'source': 'source_compiler_version_selection', 'instruction_image_offset': 1319539, 'instruction_hex': '488b35fe95c933', 'target_image_offset': 870168696}, {'source': 'source_compiler_version_selection', 'instruction_image_offset': 1319549, 'instruction_hex': 'ff157d69bc32', 'target_image_offset': 852528128}, {'source': 'source_compiler_version_selection', 'instruction_image_offset': 1319570, 'instruction_hex': '488b351f84c933', 'target_image_offset': 870164152}, {'source': 'source_compiler_version_selection', 'instruction_image_offset': 1319613, 'instruction_hex': '488b05d4f3bc32', 'target_image_offset': 852563608}, {'source': 'source_compiler_version_selection', 'instruction_image_offset': 1319633, 'instruction_hex': '488d05e2e20200', 'target_image_offset': 1508794}, {'source': 'source_compiler_version_selection', 'instruction_image_offset': 1319644, 'instruction_hex': '488d05c591c433', 'target_image_offset': 869840040}, {'source': 'source_compiler_version_selection', 'instruction_image_offset': 1319659, 'instruction_hex': '48833da5479936ff', 'target_image_offset': 917334680}, {'source': 'source_compiler_version_selection', 'instruction_image_offset': 1319669, 'instruction_hex': '8b0595479936', 'target_image_offset': 917334672}, {'source': 'source_compiler_version_selection', 'instruction_image_offset': 1319682, 'instruction_hex': 'e811710800', 'target_image_offset': 1872920}, {'source': 'source_compiler_version_selection', 'instruction_image_offset': 1319692, 'instruction_hex': 'e81f710800', 'target_image_offset': 1872944}, {'source': 'source_compiler_version_selection', 'instruction_image_offset': 1319713, 'instruction_hex': 'e810a60800', 'target_image_offset': 1886518}, {'source': 'source_library_request', 'instruction_image_offset': 1379295, 'instruction_hex': 'e832e0ecff', 'target_image_offset': 125974}, {'source': 'source_library_request', 'instruction_image_offset': 1379323, 'instruction_hex': '488d058f000000', 'target_image_offset': 1379473}, {'source': 'source_library_request', 'instruction_image_offset': 1379334, 'instruction_hex': '488d05839bc333', 'target_image_offset': 869836688}, {'source': 'source_library_request', 'instruction_image_offset': 1379377, 'instruction_hex': 'e844e5ffff', 'target_image_offset': 1372538}, {'source': 'source_library_request', 'instruction_image_offset': 1379387, 'instruction_hex': 'e8cebd0700', 'target_image_offset': 1886734}, {'source': 'source_library_request', 'instruction_image_offset': 1379417, 'instruction_hex': 'e8d2bc0700', 'target_image_offset': 1886512}, {'source': 'source_library_request', 'instruction_image_offset': 1379437, 'instruction_hex': 'e8a0bc0700', 'target_image_offset': 1886482}, {'source': 'source_library_request', 'instruction_image_offset': 1379453, 'instruction_hex': 'e8a8bc0700', 'target_image_offset': 1886506}]
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
# Exact miss path and completion pointer from the observed request body.
# Only the actual cache-miss implementation may be larger than 1KiB.
for name,offset,limit in [('source_cache_miss_compile',0x14f17a,8192),
                          ('source_cache_result_callback',0x150c91,1024),
                          ('compiler_override_policy',0xfbc91,1024),
                          ('compiler_version_default_block',0x1705ba,1024),
                          ('compiler_override_version_once',0x1c9418,1024),
                          ('compiler_override_use_device_once',0x1c9430,1024)]:
    capture(name,offset,limit)
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
for offset in (0x33ddb878,0x33dda6b8):
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
report['limits']={'total_code_bytes':MAX_TOTAL_CODE,'output_bytes':MAX_OUTPUT,'cache_miss_code_bytes':8192,'other_new_function_code_bytes':1024,'references':len(REQUESTS),'reference_pointer_bytes':8,'reference_stub_bytes':16,'reference_string_bytes':192}
report['images']={str(b):{k:v[k] for k in ('uuid','header_sha256','base','function_starts_note')} for b,v in layouts.items()}
output=json.dumps(report,indent=2,sort_keys=True)
if len(output.encode())+1>MAX_OUTPUT:raise RuntimeError('Output exceeds128KiB bound')
print(output)
