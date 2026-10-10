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
 if not name.startswith('selected_') and (loc['nearest_symbol']!=name or loc['symbol_offset']!=0):raise RuntimeError('Unexpected selected symbol')
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
capture('selected_at_0x13b6',0x13b6)
system.mach_vm_region.argtypes=[c.c_uint,c.POINTER(c.c_uint64),c.POINTER(c.c_uint64),c.c_int,c.POINTER(c.c_int),c.POINTER(c.c_uint),c.POINTER(c.c_uint)]
system.mach_vm_region.restype=c.c_int
system.mach_port_deallocate.argtypes,system.mach_port_deallocate.restype=[c.c_uint,c.c_uint],c.c_int
own_task=c.c_uint.in_dll(system,'mach_task_self_').value
def read_word(address):
 start,size,count,object_name=c.c_uint64(address),c.c_uint64(),c.c_uint(9),c.c_uint()
 info=(c.c_int*9)()
 status=system.mach_vm_region(own_task,c.byref(start),c.byref(size),9,info,c.byref(count),c.byref(object_name))
 released=system.mach_port_deallocate(own_task,object_name.value) if object_name.value else None
 if status or count.value!=9 or not(info[0]&1) or not(start.value<=address and address+8<=start.value+size.value):raise RuntimeError('GOT pointer outside actual readable mapping')
 return {'address':address,'word':struct.unpack('<Q',c.string_at(address,8))[0],'region':{'address':start.value,'bytes':size.value,'protection':info[0],'object_name_release_status':released}}
def reference(offset):
 address=base+offset;section=readable_section(address,1);result={'image_offset':offset,'location':location(address),'section':section['name']}
 if section['name']=='__cstring':
  raw=c.string_at(address,min(256,section['address']+section['bytes']-address));text,sep,_=raw.partition(b'\0')
  if not sep:raise RuntimeError('Metadata string unterminated')
  result.update(text=text.decode('utf-8'),hex=(text+b'\0').hex())
 elif section['name']=='__stubs':
  readable_section(address,6);raw=c.string_at(address,6)
  if raw[:2]!=b'\xff\x25':raise RuntimeError('Unexpected indirect stub encoding')
  pointer=address+6+int.from_bytes(raw[2:],'little',signed=True);got=read_word(pointer)
  result.update(stub_hex=raw.hex(),pointer=got,pointee=location(got['word']))
 elif section['name']=='__text':
  name='selected_tail_at_'+hex(offset);capture(name,offset);result['captured_helper']=name
 else:raise RuntimeError('Unexpected explicit reference section: '+section['name'])
 return result
report['explicit_references']={hex(offset):reference(offset) for offset in [0x6070,0x57b8,0x580c,0x4856,0x57a6,0x644b,0x5c45]}
helper=report['helpers']['selected_at_0x13b6'];raw=bytes.fromhex(helper['code_hex'])
if raw[-5:-4]==b'\xe9':
 target=helper['image_offset']+len(raw)+int.from_bytes(raw[-4:],'little',signed=True)
 report['tail_reference']=reference(target)
from pathlib import Path
import os
xattr_system=c.CDLL(None,use_errno=True)
xattr_system.getxattr.argtypes=[c.c_char_p,c.c_char_p,c.c_void_p,c.c_size_t,c.c_uint32,c.c_int]
xattr_system.getxattr.restype=c.c_ssize_t
conf=Path('/System/Library/Sandbox/rootless.conf')
if conf.stat().st_size>65536:raise RuntimeError('Rootless configuration exceeds bound')
raw=conf.read_bytes()
report['rootless_configuration']={'path':str(conf),'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest(),'selected_lines':[{"line":n,"text":line} for n,line in enumerate(raw.decode().splitlines(),1) if 'GPUBundles' in line or 'KernelExtensionManagement' in line]}
report['path_metadata']={}
for path in ['/Library/GPUBundles','/System/Library/Extensions']:
 try:
  st=os.stat(path);meta={'uid':st.st_uid,'gid':st.st_gid,'mode':oct(st.st_mode),'flags':st.st_flags}
  try:
   buffer=c.create_string_buffer(512)
   size=xattr_system.getxattr(path.encode(),b'com.apple.rootless',buffer,512,0,0)
   if size<0:raise OSError(c.get_errno(),'getxattr rejected')
   if size>512:raise RuntimeError('Rootless metadata exceeds bound')
   meta['com.apple.rootless_hex']=buffer.raw[:size].hex()
  except OSError as error:meta['rootless_xattr_error']={'errno':error.errno,'message':str(error)}
  report['path_metadata'][path]=meta
 except OSError as error:report['path_metadata'][path]={'errno':error.errno,'message':str(error)}
sdk=Path(subprocess.check_output(['xcrun','--sdk','macosx','--show-sdk-path'],text=True,timeout=10).strip())
header=sdk/'System/Library/Frameworks/Metal.framework/Headers/MTLDevice.h'
if header.stat().st_size>256*1024:raise RuntimeError('MTLDevice header exceeds bound')
raw=header.read_bytes();report['public_sdk_header']={'path':str(header),'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest(),'text':raw.decode()}
tbd=sdk/'System/Library/Frameworks/Metal.framework/Metal.tbd'
if tbd.stat().st_size+header.stat().st_size>256*1024:raise RuntimeError('SDK header and TBD exceed total bound')
raw=tbd.read_bytes();report['public_sdk_tbd']={'path':str(tbd),'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest(),'text':raw.decode()}
metal=c.CDLL('/System/Library/Frameworks/Metal.framework/Metal')
objc=c.CDLL('/usr/lib/libobjc.A.dylib')
system.dlsym.argtypes,system.dlsym.restype=[c.c_void_p,c.c_char_p],c.c_void_p
objc.objc_getClass.argtypes,objc.objc_getClass.restype=[c.c_char_p],c.c_void_p
objc.object_getClass.argtypes,objc.object_getClass.restype=[c.c_void_p],c.c_void_p
cls=objc.objc_getClass(b'_MTLDevice');meta=objc.object_getClass(cls) if cls else None
report['device_class_exports']={}
for name,expected in [('OBJC_CLASS_$__MTLDevice',cls),('OBJC_METACLASS_$__MTLDevice',meta)]:
 address=system.dlsym(metal._handle,name.encode())
 report['device_class_exports'][name]={'address':address,'runtime_address':expected,'matches_runtime':bool(address and address==expected),'location':location(address) if address else None}
report['total_code_bytes']=total
print(json.dumps(report,indent=2,sort_keys=True))
