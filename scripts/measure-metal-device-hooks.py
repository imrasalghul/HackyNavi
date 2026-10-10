#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fragment after measure-metal-initializers.py definitions, no target calls."""
EXPECTED_METAL_UUID = "c3efbabe-6b06-33ab-bc45-43fe86297aa1"
METHODS = [('_MTLDevice', 'initLimits'), ('_MTLDevice', 'initFeatureQueries'),
           ('_MTLDevice', 'initWorkarounds'), ('_MTLDevice', 'allowLibrariesFromOtherPlatforms'),
           ('MTLIOAccelDevice', '_setAcceleratorService:'), ('MTLIOAccelService', 'dealloc')]
OFFSETS = [0xac51, 0x19f0a9]
report = {'system': subprocess.check_output(['sw_vers'], text=True, timeout=10).strip(),
          'helpers': {}, 'images': {}, 'protocols': {}, 'classes': {},
          'device_enumerated': False, 'device_instantiated': False,
          'objc_messages_sent': False, 'user_client_opened': False,
          'gpu_submitted': False, 'remote_files_created': False,
          'limits': {'method_code_bytes': 16384, 'total_code_bytes': 32768,
                     'protocol_methods': 512, 'class_methods': 128, 'metadata_string_bytes': 512}}
layouts = {}
total = 0


def capture(name, address, encoding=None):
    global total
    loc = location(address)
    if not loc['resolved']:
        raise RuntimeError('Selected function has no image')
    base = loc['base']
    if base not in layouts:
        layouts[base] = image_layout(base)
    layout = layouts[base]
    if '/Metal.framework/' in loc['image'] and layout['uuid'] != EXPECTED_METAL_UUID:
        raise RuntimeError('Metal image changed')
    item = dict(loc, implementation=address, image_uuid=layout['uuid'])
    if encoding is not None:
        item['encoding'] = encoding
    section = next((s for s in layout['sections'] if s['name'] == '__text' and
                    s['segment'] == '__TEXT' and s['address'] <= address < s['address'] + s['bytes']), None)
    following = next((f for f in layout['functions'] if f > address), None)
    if not section or address not in layout['functions'] or not following:
        item['skipped'] = 'exact function bounds unavailable'
    else:
        count = min(following, section['address'] + section['bytes']) - address
        if count > 16384 or total + count > 32768:
            item['skipped'] = 'explicit code bound'; item['declared_bytes'] = count
        else:
            data = c.string_at(address, count); total += count
            item.update(bytes=count, sha256=hashlib.sha256(data).hexdigest(),
                        function_start_matched=True, function_end_bounded=True,
                        truncated=False, code_hex=data.hex())
    report['helpers'][name] = item


for classname, selector in METHODS:
    cls = objc.objc_getClass(classname.encode())
    method = objc.class_getInstanceMethod(cls, objc.sel_registerName(selector.encode())) if cls else None
    if not method:
        report['helpers'][classname + '::' + selector] = {'present': False}
        continue
    capture(classname + '::' + selector, objc.method_getImplementation(method),
            objc.method_getTypeEncoding(method).decode())
base = location(c.cast(metal.MTLCopyAllDevices, c.c_void_p).value)['base']
for offset in OFFSETS:
    capture('selected_at_' + hex(offset), base + offset)
sandbox = c.CDLL('/usr/lib/system/libsystem_sandbox.dylib')
capture('gpu_bundle_find_trusted', c.cast(sandbox.gpu_bundle_find_trusted, c.c_void_p).value)

for name, args, result in (
    ('objc_getProtocol', [c.c_char_p], c.c_void_p),
    ('class_conformsToProtocol', [c.c_void_p, c.c_void_p], c.c_bool),
    ('class_getSuperclass', [c.c_void_p], c.c_void_p),
    ('class_getName', [c.c_void_p], c.c_char_p),
    ('class_getInstanceSize', [c.c_void_p], c.c_size_t),
    ('class_copyMethodList', [c.c_void_p, c.POINTER(c.c_uint)], c.POINTER(c.c_void_p)),
    ('method_getName', [c.c_void_p], c.c_void_p),
    ('sel_getName', [c.c_void_p], c.c_char_p),
):
    fn = getattr(objc, name); fn.argtypes, fn.restype = args, result
system.free.argtypes = [c.c_void_p]; system.free.restype = None


class MethodDescription(c.Structure):
    _fields_ = [('name', c.c_void_p), ('types', c.c_char_p)]


objc.protocol_copyMethodDescriptionList.argtypes = [c.c_void_p, c.c_bool, c.c_bool, c.POINTER(c.c_uint)]
objc.protocol_copyMethodDescriptionList.restype = c.POINTER(MethodDescription)


def bounded(value):
    if value is None or len(value) > 512:
        raise RuntimeError('Metadata text exceeds bound')
    return value.decode()


for classname in ('_MTLDevice', 'MTLIOAccelDevice', 'MTLIOAccelService'):
    cls = objc.objc_getClass(classname.encode())
    if not cls:
        raise RuntimeError('Expected actual class missing')
    superclass = objc.class_getSuperclass(cls)
    item = {'location': location(cls), 'instance_bytes': objc.class_getInstanceSize(cls),
            'superclass': bounded(objc.class_getName(superclass)) if superclass else None,
            'conforms': {}}
    for name in ('MTLDevice', 'MTLDeviceSPI'):
        protocol = objc.objc_getProtocol(name.encode())
        if not protocol:
            raise RuntimeError('Expected publication protocol missing')
        item['conforms'][name] = bool(objc.class_conformsToProtocol(cls, protocol))
    if classname == 'MTLIOAccelService':
        count = c.c_uint(0); methods = objc.class_copyMethodList(cls, c.byref(count))
        try:
            if count.value > 128: raise RuntimeError('Class method list exceeds bound')
            item['own_methods'] = [{'selector': bounded(objc.sel_getName(objc.method_getName(methods[n]))),
                                    'encoding': bounded(objc.method_getTypeEncoding(methods[n])),
                                    'implementation': location(objc.method_getImplementation(methods[n]))}
                                   for n in range(count.value)]
        finally:
            system.free(methods)
    report['classes'][classname] = item
for name in ('MTLDeviceSPI',):
    protocol = objc.objc_getProtocol(name.encode())
    groups = {}
    for required in (True, False):
        for instance in (True, False):
            count = c.c_uint(0)
            methods = objc.protocol_copyMethodDescriptionList(protocol, required, instance, c.byref(count))
            try:
                if count.value > 512: raise RuntimeError('Protocol method list exceeds bound')
                groups[('required' if required else 'optional') + ('_instance' if instance else '_class')] = [
                    {'selector': bounded(objc.sel_getName(methods[n].name)), 'encoding': bounded(methods[n].types)}
                    for n in range(count.value)]
            finally:
                system.free(methods)
    report['protocols'][name] = groups
report['total_code_bytes'] = total
for base, layout in layouts.items():
    report['images'][str(base)] = {k: v for k, v in layout.items() if k != 'functions'}
print(json.dumps(report, indent=2, sort_keys=True))
