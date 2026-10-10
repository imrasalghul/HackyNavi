#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read bounded actual GPU bundle policy code/data; never execute policy or GPU code.

Run via python3 stdin. No installation, Objective-C object/message, user client,
GPU access or remote file creation. Only dyld metadata and known code are read.
"""
import ctypes as c
import hashlib,json,platform,struct,subprocess,uuid
if platform.system()!='Darwin' or platform.machine()!='x86_64':raise RuntimeError('Intel macOS required')
system=c.CDLL(None)
sandbox=c.CDLL('/usr/lib/system/libsystem_sandbox.dylib')
class DlInfo(c.Structure):
    _fields_ = [("name", c.c_char_p), ("base", c.c_void_p),
                ("symbol", c.c_char_p), ("symbol_address", c.c_void_p)]

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

system.dladdr.argtypes,system.dladdr.restype=[c.c_void_p,c.POINTER(DlInfo)],c.c_int
EXPECTED_UUID='4f88fd83-3d9b-30c6-aac7-f9d3d2ace5b9'
base=location(c.cast(sandbox.gpu_bundle_find_trusted,c.c_void_p).value)['base']
layout=image_layout(base)
if layout['uuid']!=EXPECTED_UUID:raise RuntimeError('Actual sandbox image changed')
report={'system':subprocess.check_output(['sw_vers'],text=True,timeout=10).strip(),
 'image':{k:v for k,v in layout.items() if k!='functions'},'helpers':{},'paths':[],
 'policy_called':False,'metal_object_created':False,'gpu_executed':False,'remote_files_created':False,
 'limits':{'max_code_bytes':16384,'path_count':2,'max_path_bytes':256}}
total=0
def readable_section(address,count):
 segment=next((s for s in layout['segments'] if s['initprot']&1 and s['address']<=address and address+count<=s['address']+s['bytes']),None)
 section=next((s for s in layout['sections'] if s['address']<=address and address+count<=s['address']+s['bytes']),None)
 if not segment or not section:raise RuntimeError('Selected read outside known readable image section')
 return section
def capture(name,offset):
 global total
 address=base+offset
 section=readable_section(address,1)
 if section['name']!='__text' or address not in layout['functions']:raise RuntimeError('Not exact function start')
 following=next((p for p in layout['functions'] if p>address),None)
 if following is None or following>section['address']+section['bytes']:raise RuntimeError('Unbounded function')
 count=following-address
 if total+count>16384:raise RuntimeError('Code bound exceeded')
 raw=c.string_at(address,count);total+=count
 loc=location(address)
 if loc['nearest_symbol']!=name or loc['symbol_offset']!=0:raise RuntimeError('Unexpected selected symbol')
 report['helpers'][name]=dict(loc,bytes=count,code_hex=raw.hex(),sha256=hashlib.sha256(raw).hexdigest(),image_uuid=layout['uuid'],function_start_matched=True,function_end_bounded=True,truncated=False)
capture('gpu_bundle_find_trusted',0x11ff)
if report['helpers']['gpu_bundle_find_trusted']['sha256']!='4045241086bbdac0401d6da88cf5ebdffdb5413ff639c6cca106b57cf95efd46':raise RuntimeError('Find function bytes changed')
capture('gpu_bundle_is_path_trusted',0x13c8)
# Actual indexed read + cmp$16 in verified find_trusted is exactly two pointers.
array=base+0x32d12120
section=readable_section(array,16)
if section['name']!='__const' or section['segment']!='__DATA_CONST':raise RuntimeError('Unexpected path array section')
raw=c.string_at(array,16)
report['path_array']={'image_offset':array-base,'bytes':16,'hex':raw.hex(),'sha256':hashlib.sha256(raw).hexdigest()}
for index,pointer in enumerate(struct.unpack('<QQ',raw)):
 section=readable_section(pointer,1)
 if section['name']!='__cstring':raise RuntimeError('Path pointer not actual cstring')
 count=min(256,section['address']+section['bytes']-pointer)
 raw=c.string_at(pointer,count);text,sep,_=raw.partition(b'\0')
 if not sep:raise RuntimeError('Path string unterminated')
 report['paths'].append({'index':index,'image_offset':pointer-base,'text':text.decode('utf-8'),'hex':(text+b'\0').hex()})
report['total_code_bytes']=total
print(json.dumps(report,indent=2,sort_keys=True))
