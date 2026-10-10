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

# The reader above is reused from the project's previously reviewed initializer
# measurement. No method implementation is called; code bytes are only data.
EXPECTED_METAL_UUID = "c3efbabe-6b06-33ab-bc45-43fe86297aa1"
MAX_METHOD_BYTES, MAX_CODE_BYTES, MAX_OUTPUT_BYTES = 1024, 16384, 131072
MAX_CLASSES, MAX_RELEVANT_CLASSES, MAX_OWN_METHODS = 32768, 48, 1024
MAX_CATALOG_ENTRIES = 160
for name, args, result in (
    ('objc_getClassList', [c.POINTER(c.c_void_p), c.c_int], c.c_int),
    ('class_getName', [c.c_void_p], c.c_void_p),
    ('class_getSuperclass', [c.c_void_p], c.c_void_p),
    ('class_copyMethodList', [c.c_void_p, c.POINTER(c.c_uint)], c.POINTER(c.c_void_p)),
    ('method_getName', [c.c_void_p], c.c_void_p),
    ('sel_getName', [c.c_void_p], c.c_void_p),
    ('method_getTypeEncoding', [c.c_void_p], c.c_void_p),
):
    fn = getattr(objc, name); fn.argtypes, fn.restype = args, result
system.free.argtypes, system.free.restype = [c.c_void_p], None

def metadata(address):
    if not address:
        return None
    data = bytearray()
    for index in range(512):
        value = c.c_ubyte.from_address(address + index).value
        if not value:
            return data.decode('utf-8')
        data.append(value)
    raise RuntimeError('Metadata string exceeds 512-byte bound')

report = {'system': subprocess.check_output(['sw_vers'], text=True, timeout=10).strip(),
          'machine': platform.machine(), 'helpers': {}, 'images': {}, 'classes': {},
          'relevant_method_catalog': [],
          'limits': {'one_method_code_bytes': MAX_METHOD_BYTES, 'total_code_bytes': MAX_CODE_BYTES,
                     'total_output_bytes': MAX_OUTPUT_BYTES, 'loaded_class_count': MAX_CLASSES,
                     'relevant_class_count': MAX_RELEVANT_CLASSES, 'catalog_entries': MAX_CATALOG_ENTRIES},
          'device_enumerated': False, 'device_instantiated': False, 'objc_messages_sent': False,
          'compiler_invoked': False, 'user_client_opened': False, 'gpu_submitted': False,
          'remote_files_created': False,
          'scope': 'Loaded class/method metadata and bounded named code only; no Objective-C allocation or method invocation.'}
layouts, captured = {}, {}
total = 0

def capture(classname, selector):
    global total
    name = classname + '::' + selector
    cls = objc.objc_getClass(classname.encode())
    method = objc.class_getInstanceMethod(cls, objc.sel_registerName(selector.encode())) if cls else None
    if not method:
        report['helpers'][name] = {'present': False}
        return
    address = objc.method_getImplementation(method)
    loc = location(address)
    item = dict(loc, present=True, encoding=metadata(objc.method_getTypeEncoding(method)))
    report['helpers'][name] = item
    if not loc['resolved']:
        item['skipped'] = 'implementation image unresolved'; return
    if not loc['image'].startswith('/System/Library/'):
        item['skipped'] = 'outside actual system framework'; return
    base = loc['base']
    if base not in layouts:
        if len(layouts) >= 4: raise RuntimeError('Too many selected images')
        layouts[base] = image_layout(base)
    layout = layouts[base]
    if '/Metal.framework/' in loc['image'] and layout['uuid'] != EXPECTED_METAL_UUID:
        raise RuntimeError('Actual Metal image changed; review new identity first')
    item['image_uuid'] = layout['uuid']
    if address in captured:
        item['same_implementation_as'] = captured[address]; return
    section = next((s for s in layout['sections'] if s['name'] == '__text' and s['segment'] == '__TEXT'
                    and s['address'] <= address < s['address'] + s['bytes']), None)
    following = next((f for f in layout['functions'] if f > address), None)
    if not section or address not in layout['functions'] or following is None:
        item['skipped'] = 'exact function bounds unavailable'; return
    count = min(following, section['address'] + section['bytes']) - address
    item['declared_function_bytes'] = count
    if count <= 0 or count > MAX_METHOD_BYTES or total + count > MAX_CODE_BYTES:
        item['skipped'] = 'explicit code bound'; return
    data = c.string_at(address, count)
    total += count; captured[address] = name
    item.update(bytes=count, sha256=hashlib.sha256(data).hexdigest(), code_hex=data.hex(),
                function_start_matched=True, function_end_bounded=True, truncated=False)

device_selectors = ('newLibraryWithSource:options:error:',
                    'newLibraryWithSource:options:completionHandler:',
                    'newLibraryWithData:error:', 'compiler', 'getCompilerConnectionManager:',
                    'targetDeviceArchitecture', 'getMostCompatibleArchitecture',
                    'newCompilerWithDescriptor:error:')
for classname in ('_MTLDevice', 'MTLIOAccelDevice'):
    for selector in device_selectors:
        capture(classname, selector)

# objc_getClassList returns Class metadata, not created device/library objects.
# A bounded second call tolerates a loaded-class count change by rejecting it.
count = objc.objc_getClassList(None, 0)
if count <= 0 or count > MAX_CLASSES:
    raise RuntimeError('Loaded class count exceeds explicit bound')
classes = (c.c_void_p * count)()
actual = objc.objc_getClassList(classes, count)
if actual < 0 or actual > count:
    raise RuntimeError('Loaded class inventory changed')
selected = []
exact_classes = {'MTLFunction', 'MTLFunctionInternal', '_MTLFunction',
                 '_MTLFunctionInternal', 'MTLTargetDeviceArchitecture'}
class_prefixes = ('MTLLibrary', '_MTLLibrary', 'MTLCompiler', '_MTLCompiler')
for index in range(actual):
    name = metadata(objc.class_getName(classes[index]))
    if name and (name in exact_classes or name.startswith(class_prefixes)):
        selected.append((name, classes[index]))
selected.sort()
report['class_selection'] = {'loaded_classes': actual, 'matched_classes': len(selected),
                           'exact_names': sorted(exact_classes), 'prefixes': list(class_prefixes),
                           'captured_classes': min(len(selected), MAX_RELEVANT_CLASSES),
                           'skipped_classes': max(0, len(selected) - MAX_RELEVANT_CLASSES),
                           'complete': len(selected) <= MAX_RELEVANT_CLASSES}
report['catalog_skipped_entries'] = 0
for name, cls in selected[:MAX_RELEVANT_CLASSES]:
    parent = objc.class_getSuperclass(cls)
    report['classes'][name] = {'location': location(cls),
                              'superclass': metadata(objc.class_getName(parent)) if parent else None}
    amount = c.c_uint(0)
    methods = objc.class_copyMethodList(cls, c.byref(amount))
    try:
        if amount.value > MAX_OWN_METHODS:
            raise RuntimeError('Class method count exceeds bound')
        for index in range(amount.value):
            method = methods[index]
            selector = metadata(objc.sel_getName(objc.method_getName(method)))
            if not selector or not any(token in selector.lower() for token in ('source', 'library', 'bitcode', 'compiler', 'targetdata')):
                continue
            if len(report['relevant_method_catalog']) >= MAX_CATALOG_ENTRIES:
                report['catalog_skipped_entries'] += 1
                continue
            report['relevant_method_catalog'].append({'class': name, 'selector': selector,
                'encoding': metadata(objc.method_getTypeEncoding(method)),
                'location': location(objc.method_getImplementation(method))})
            if selector in ('libraryDataContents', 'bitcodeData',
                            'initWithTargetData:cacheUUID:pluginPath:device:compilerFlags:'):
                capture(name, selector)
    finally:
        system.free(methods)

report['total_code_bytes'] = total
for base, layout in layouts.items():
    # UUID and header/load-command digest identify the actual mapped image.
    # This intentionally does not copy or hash the bulk system framework.
    report['images'][str(base)] = {key: layout[key] for key in ('uuid', 'header_sha256', 'base', 'function_starts_note')}
output = json.dumps(report, indent=2, sort_keys=True)
if len(output.encode('utf-8')) + 1 > MAX_OUTPUT_BYTES:
    raise RuntimeError('Output exceeds explicit 128 KiB bound')
print(output)


