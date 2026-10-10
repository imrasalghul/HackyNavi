#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read bounded runtime-MSL compiler metadata and named method code.

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

EXPECTED = {'_MTLDevice::newLibraryWithSource:options:error:': {'offset': 1036906, 'sha256': '853622fe3434cd9319cfbd6a020a9af82b46a195b96d0c77bd61a20350f71ff1'}, '_MTLDevice::newLibraryWithSource:options:completionHandler:': {'offset': 1037331, 'sha256': '8d41423e9cafe1b6429474e65be94115cdc8394f059b11075291db51e75343eb'}, '_MTLDevice::newLibraryWithData:error:': {'offset': 179619, 'sha256': '61d3eb1f1de038b06a356bc0df45dfb082e071b0a4c182f7fd26202cacb4785e'}, '_MTLLibrary::libraryDataContents': {'offset': 1367956, 'sha256': '7a3f95ff7586f155bd0fffd9e90962de8061d4c2feb697d19ec17628381dfcff'}}
REQUESTS = [{'source': '_MTLDevice::newLibraryWithSource:options:error:', 'instruction_image_offset': 1036953, 'target_image_offset': 1035093}, {'source': '_MTLDevice::newLibraryWithSource:options:error:', 'instruction_image_offset': 1036964, 'target_image_offset': 1035116}, {'source': '_MTLDevice::newLibraryWithSource:options:completionHandler:', 'instruction_image_offset': 1037349, 'target_image_offset': 852563608}, {'source': '_MTLDevice::newLibraryWithSource:options:error:', 'instruction_image_offset': 1037039, 'target_image_offset': 126569}, {'source': '_MTLDevice::newLibraryWithSource:options:error:', 'instruction_image_offset': 1037050, 'target_image_offset': 869829024}, {'source': '_MTLDevice::newLibraryWithSource:options:completionHandler:', 'instruction_image_offset': 1037401, 'target_image_offset': 1379888}, {'source': '_MTLDevice::newLibraryWithSource:options:error:', 'instruction_image_offset': 1037108, 'target_image_offset': 1037262}, {'source': '_MTLDevice::newLibraryWithSource:options:error:', 'instruction_image_offset': 1037162, 'target_image_offset': 852528240}, {'source': '_MTLDevice::newLibraryWithSource:options:error:', 'instruction_image_offset': 1037152, 'target_image_offset': 1887046}, {'source': '_MTLDevice::newLibraryWithSource:options:error:', 'instruction_image_offset': 1037248, 'target_image_offset': 1885846}, {'source': '_MTLDevice::newLibraryWithSource:options:error:', 'instruction_image_offset': 1037256, 'target_image_offset': 1885870}, {'source': '_MTLDevice::newLibraryWithSource:options:completionHandler:', 'instruction_image_offset': 1037372, 'target_image_offset': 1037412}, {'source': '_MTLDevice::newLibraryWithSource:options:completionHandler:', 'instruction_image_offset': 1037383, 'target_image_offset': 869829072}, {'source': '_MTLDevice::newLibraryWithData:error:', 'instruction_image_offset': 179639, 'target_image_offset': 179716}, {'source': '_MTLDevice::newLibraryWithData:error:', 'instruction_image_offset': 179647, 'target_image_offset': 870164224}, {'source': '_MTLDevice::newLibraryWithData:error:', 'instruction_image_offset': 179657, 'target_image_offset': 852528128}, {'source': '_MTLDevice::newLibraryWithData:error:', 'instruction_image_offset': 179701, 'target_image_offset': 1710884}, {'source': '_MTLLibrary::libraryDataContents', 'instruction_image_offset': 1367960, 'target_image_offset': 917288640}]
for name,expected in EXPECTED.items():
    capture(name,expected['offset'])
    if report['helpers'][name].get('sha256')!=expected['sha256']:
        raise RuntimeError('Previously captured source method changed')
# Only the actual builder callee is allowed more than1KiB, if its observed
# function-start extent requires it. Total code/output bounds stay unchanged.
for name,offset,limit in [('source_library_builder',0x150e30,8192),
                          ('synchronous_source_completion',0x1ee69,1024),
                          ('asynchronous_source_completion',0xfd464,1024),
                          ('synchronous_error_helper',0xfd3ce,1024),
                          ('owned_library_builder_constructor',1070290,1024),
                          ('owned_library_builder_destructor',1070406,1024)]:
    capture(name,offset,limit)
for classname,selector in [('_MTLLibrary','initWithLibraryContainer:device:'),('_MTLLibrary','dealloc')]:
    cls=objc.objc_getClass(classname.encode());method=objc.class_getInstanceMethod(cls,objc.sel_registerName(selector.encode())) if cls else None
    name=classname+'::'+selector
    if not method:report['helpers'][name]={'present':False};continue
    address=objc.method_getImplementation(method)
    capture(name,address-base)
    report['helpers'][name]['encoding']=objc.method_getTypeEncoding(method).decode()
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
for name,args,result in [('class_copyIvarList',[c.c_void_p,c.POINTER(c.c_uint)],c.POINTER(c.c_void_p)),
                         ('ivar_getName',[c.c_void_p],c.c_void_p),('ivar_getTypeEncoding',[c.c_void_p],c.c_void_p),
                         ('ivar_getOffset',[c.c_void_p],c.c_ssize_t)]:
    fn=getattr(objc,name);fn.argtypes,fn.restype=args,result
system.free.argtypes,system.free.restype=[c.c_void_p],None
for classname in ('_MTLDevice','_MTLLibrary'):
    cls=objc.objc_getClass(classname.encode());count=c.c_uint(0)
    ivars=objc.class_copyIvarList(cls,c.byref(count)) if cls else None
    report['ivars'][classname]={'declared_count':count.value,'selected':[]}
    try:
        if count.value>128:raise RuntimeError('Ivar metadata count exceeds bound')
        for i in range(count.value):
            name=bounded_cstring(objc.ivar_getName(ivars[i]))
            if classname=='_MTLDevice' and not any(k in (name['text'] or '').lower() for k in ('compiler','library')):continue
            report['ivars'][classname]['selected'].append({'name':name,'encoding':bounded_cstring(objc.ivar_getTypeEncoding(ivars[i])), 'offset':objc.ivar_getOffset(ivars[i])})
    finally:system.free(ivars)
report['total_code_bytes']=total
report['limits']={'total_code_bytes':MAX_TOTAL_CODE,'output_bytes':MAX_OUTPUT,'builder_only_code_bytes':8192,'other_function_code_bytes':1024,'references':len(REQUESTS),'reference_pointer_bytes':8,'reference_stub_bytes':16,'reference_string_bytes':192}
report['images']={str(b):{k:v[k] for k in ('uuid','header_sha256','base','function_starts_note')} for b,v in layouts.items()}
output=json.dumps(report,indent=2,sort_keys=True)
if len(output.encode())+1>MAX_OUTPUT:raise RuntimeError('Output exceeds128KiB bound')
print(output)
