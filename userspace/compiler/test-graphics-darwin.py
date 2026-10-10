#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Actual Apple AIR/metallib -> explicit tools -> NIR/ACO CLI and C ABI checks."""
import ctypes as c
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    stage = Path(sys.argv[1]).resolve()
    output = stage / 'results/compiler'
    output.mkdir(parents=True)
    report = {'success': False, 'checks': 0, 'gpu_executed': False, 'metal_objects_created': False,
              'products': [], 'commands': []}

    def check(value, message):
        report['checks'] += 1
        if not value:
            raise AssertionError(message)

    def run(args, expected=0):
        result = subprocess.run([str(x) for x in args], capture_output=True, text=True, timeout=30)
        report['commands'].append({'args': [str(x) for x in args], 'returncode': result.returncode,
                                   'stdout': result.stdout, 'stderr': result.stderr})
        check(result.returncode == expected, result.stderr)
        return result

    def module(name):
        spec = importlib.util.spec_from_file_location(name, stage / ('scripts/verify-' + name + '.py'))
        value = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(value)
        return value

    verifiers = {'vertex': module('ngg'), 'fragment': module('fragment'),
                 'fragment_buffer': module('fragment-buffer'), 'vertex_buffer': module('vertex-buffer')}
    compiler = c.CDLL(str(stage / 'libhackynavi_metal_compiler.dylib'))
    graphics = c.CDLL(str(stage / 'libHackyNaviGraphicsCompiler.dylib'))
    ptr, size, u32 = c.c_void_p, c.c_size_t, c.c_uint32

    class Tools(c.Structure):
        _fields_ = [('struct_size', u32), ('version', u32), ('air_dis_path', ptr), ('spirv_val_path', ptr)]

    compiler.hn_shader_compile_with_tools.argtypes = [ptr, ptr, size, u32, u32, c.c_char_p, u32, u32, u32, u32, u32]
    compiler.hn_shader_compile_with_tools.restype = ptr
    for name, arguments, result in [('error', [ptr], c.c_char_p), ('words', [ptr, c.POINTER(size)], ptr),
                                    ('spirv_entry_point', [ptr], c.c_char_p), ('reflection', [ptr], c.c_char_p)]:
        fn = getattr(compiler, 'hn_shader_result_' + name)
        fn.argtypes, fn.restype = arguments, result
    compiler.hn_shader_result_free.argtypes = [ptr]
    compiler.hn_shader_result_free.restype = None
    functions = {}
    for kind, suffix in [('vertex', 'ngg'), ('fragment', 'fragment'),
                         ('fragment_buffer', 'fragment_buffer'), ('vertex_buffer', 'vertex_buffer')]:
        fn = getattr(graphics, 'hn_gfx11_' + suffix + '_compile_validated_file')
        fn.argtypes, fn.restype = [c.c_char_p] * 3, c.c_int
        functions[kind] = fn
    air = c.create_string_buffer(os.fsencode(stage / 'apple-air-dis'))
    val = c.create_string_buffer(os.fsencode(stage / 'spirv-val'))
    tools = Tools(c.sizeof(Tools), 1, c.cast(air, ptr), c.cast(val, ptr))
    baseline = json.loads((stage / 'fixtures/apple-baseline.json').read_text())
    buffer_baseline = json.loads((stage / 'fixtures/buffer-baseline.json').read_text())
    vertex_baseline = json.loads((stage / 'fixtures/vertex-buffer-baseline.json').read_text())
    identity = {}
    try:
        for ext, fmt in [('air', 0), ('metallib', 2)]:
            for kind, stage_id, entry, suffix in [('vertex', 1, 'hn_triangle_vertex', 'ngg'),
                                                  ('fragment', 2, 'hn_constant_fragment', 'fragment'),
                                                  ('fragment_buffer', 2, 'hn_buffer_fragment', 'fragment-buffer'),
                                                  ('vertex_buffer', 1, 'hn_buffer_vertex', 'vertex-buffer')]:
                fixture = suffix if kind.endswith('_buffer') else 'graphics'
                raw = (stage / ('fixtures/' + fixture + '.' + ext)).read_bytes()
                memory = c.create_string_buffer(raw)
                directory = output / (ext + '-' + kind)
                directory.mkdir()
                result = compiler.hn_shader_compile_with_tools(c.byref(tools), memory, len(raw), fmt,
                                                                stage_id, entry.encode(), 1, 1, 1, 0, 1)
                try:
                    check(bool(result), 'Owned actual shader result')
                    error = compiler.hn_shader_result_error(result).decode()
                    check(not error, 'Actual Apple translation: ' + error)
                    count = size()
                    words = compiler.hn_shader_result_words(result, c.byref(count))
                    check(bool(words) and 5 < count.value <= 16 * 1024 * 1024, 'Actual bounded SPIR-V')
                    spirv = directory / 'shader.spv'
                    spirv.write_bytes(c.string_at(words, count.value * 4))
                    reflection = json.loads(compiler.hn_shader_result_reflection(result))
                    actual_entry = compiler.hn_shader_result_spirv_entry_point(result)
                    check(reflection['entry_point'] == entry and reflection['stage'].lower() == kind.split('_')[0],
                          'Actual stage/name reflection')
                    if kind == 'fragment_buffer':
                        bindings = reflection['bindings']
                        check(len(bindings) == 1 and bindings[0]['descriptor'] == {'set': 0, 'binding': 0, 'count': 1} and
                              bindings[0]['access'] == 'ReadOnly' and bindings[0]['declared_size'] == 16 and
                              bindings[0]['footprint'] == {'static_ranges': [{'offset': 0, 'size': 16}],
                                                          'strided_accesses': [], 'has_unbounded_access': False},
                              'Actual one readonly16-byte buffer interface')
                    elif kind == 'vertex_buffer':
                        bindings = reflection['bindings']
                        footprint = {'static_ranges': [], 'strided_accesses': [
                            {'base_offset': offset, 'access_size': 4,
                             'terms': [{'source': 'VertexIndex', 'stride': 16}]} for offset in (0, 4, 8, 12)],
                            'has_unbounded_access': False}
                        check(len(bindings) == 1 and bindings[0]['descriptor'] == {'set': 0, 'binding': 0, 'count': 1} and
                              bindings[0]['kind'] == 'Buffer' and bindings[0]['metal_index'] == 0 and
                              bindings[0]['access'] == 'ReadOnly' and bindings[0]['declared_size'] == 16 and
                              bindings[0]['extent'] == 'Unbounded' and bindings[0]['footprint'] == footprint and
                              reflection['vertex_builtins'] == {'uses_vertex_index': True, 'uses_instance_index': False,
                                                                 'writes_position': True},
                              'Actual VertexIndex-strided interface; 48-byte bound belongs to the later three-vertex draw')
                    else:
                        check(not reflection['bindings'], 'Supported resource-free interface')
                    run([stage / 'spirv-val', '--target-env', 'vulkan1.2', spirv])
                    results = []
                    for mode in ('cli', 'cabi'):
                        product = directory / mode
                        product.mkdir()
                        if mode == 'cli':
                            run([stage / ('hackynavi-aco-' + suffix), spirv, actual_entry.decode(), product])
                        else:
                            check(functions[kind](os.fsencode(spirv), actual_entry, os.fsencode(product)) == 0,
                                  'Actual exported stage C ABI compiles')
                        code = (product / 'shader.bin').read_bytes()
                        metadata = json.loads((product / 'shader.json').read_text())
                        decoded = verifiers[kind].verify(code, metadata)
                        (product / 'decoded.json').write_text(json.dumps(decoded, indent=2) + '\n')
                        code_hash = hashlib.sha256(code).hexdigest()
                        if kind == 'vertex_buffer':
                            expected = next(x for x in vertex_baseline['products'] if x['input_format'] == ext and x['route'] == mode)
                        elif kind == 'fragment_buffer':
                            expected = next(x for x in buffer_baseline['products'] if x['input_format'] == ext)
                        else:
                            expected = next(x for x in baseline['products'] if x['stage'] == kind and x['input_format'] == ext)
                        check(code_hash == expected['code_sha256'] and metadata == expected['metadata'],
                              'Native actual code/metadata match independently generated Windows product')
                        results.append((code, metadata))
                        report['products'].append({'format': ext, 'stage': kind, 'mode': mode,
                                                   'code_sha256': code_hash, 'bytes': len(code)})
                    check(results[0] == results[1], 'CLI and actual C ABI agree')
                    if ext == 'air':
                        identity[kind] = results[0]
                    else:
                        check(identity[kind] == results[0], 'Actual AIR and metallib agree')
                finally:
                    if result:
                        compiler.hn_shader_result_free(result)
        descriptor = graphics.hn_gfx11_fragment_buffer_descriptor
        descriptor.argtypes, descriptor.restype = [c.c_uint64, c.POINTER(u32)], c.c_int
        for address in (16, 4096, 0x1234567890, (1 << 48) - 16):
            words = (u32 * 4)(0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef)
            check(descriptor(address, words) == 0 and list(words) == [address & 0xffffffff, address >> 32, 16, 0x30016fac],
                  'Actual native Mesa raw descriptor layout')
        for address in (0, 1, 17, 1 << 48, (1 << 64) - 1):
            words = (u32 * 4)(1, 2, 3, 4)
            check(descriptor(address, words) == 2 and list(words) == [1, 2, 3, 4], 'C ABI invalid descriptor preserves output')
        check(descriptor(16, None) == 2, 'C ABI null descriptor output rejects')
        vertex_descriptor = graphics.hn_gfx11_vertex_buffer_descriptor
        vertex_descriptor.argtypes, vertex_descriptor.restype = [c.c_uint64, c.POINTER(u32)], c.c_int
        report['vertex_descriptors'] = []
        for item in vertex_baseline['actual_mesa_descriptors']:
            address = item['address']
            words = (u32 * 4)(0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef)
            expected = [address & 0xffffffff, address >> 32, 48, 0x30016fac]
            check(vertex_descriptor(address, words) == 0 and list(words) == item['descriptor'] == expected,
                  'Actual native Mesa raw48 descriptor matches independently generated Windows descriptor')
            actual = run([stage / 'hackynavi-aco-vertex-buffer', '--descriptor', hex(address)])
            check(json.loads(actual.stdout) == expected, 'Native descriptor CLI and C ABI agree')
            report['vertex_descriptors'].append({'address': address, 'descriptor': list(words)})
        for address in (0, 1, 15, 17, (1 << 48) - 32, (1 << 48) - 16, 1 << 48, (1 << 64) - 1):
            words = (u32 * 4)(1, 2, 3, 4)
            check(vertex_descriptor(address, words) == 2 and list(words) == [1, 2, 3, 4],
                  'Invalid raw48 descriptor leaves output unchanged')
        check(vertex_descriptor(16, None) == 2, 'Vertex descriptor null output rejects')
        for kind in ('vertex', 'fragment', 'fragment_buffer'):
            spirv = stage / ('fixtures/unsupported-' + kind + '.spv')
            run([stage / 'spirv-val', '--target-env', 'vulkan1.2', spirv])
            product = output / ('unsupported-' + kind)
            product.mkdir()
            check(functions[kind](os.fsencode(spirv), b'main', os.fsencode(product)) != 0,
                  'Actual valid unsupported stage fails')
            check(not (product / 'shader.bin').exists(), 'No successful code on unsupported stage')
        report['vertex_negative_inputs'] = []
        for item in vertex_baseline['negative_inputs']:
            label = item['label']
            spirv = stage / ('fixtures/unsupported-vertex-buffer-' + label + '.spv')
            check(hashlib.sha256(spirv.read_bytes()).hexdigest() == item['spirv_sha256'],
                  'Exact independently validated unsupported vertex program')
            run([stage / 'spirv-val', '--target-env', 'vulkan1.2', spirv])
            for mode in ('cli', 'cabi'):
                product = output / ('unsupported-vertex-buffer-' + label + '-' + mode)
                product.mkdir()
                if mode == 'cli':
                    run([stage / 'hackynavi-aco-vertex-buffer', spirv, 'main', product], 2)
                else:
                    check(functions['vertex_buffer'](os.fsencode(spirv), b'main', os.fsencode(product)) == 2,
                          'Actual C ABI rejects unsupported vertex buffer semantics')
                check(not (product / 'shader.bin').exists() and not (product / 'shader.json').exists(),
                      'Unsupported vertex buffer program publishes no successful product')
            report['vertex_negative_inputs'].append({'label': label, 'spirv_sha256': item['spirv_sha256']})
        report['success'] = True
    finally:
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'success': True, 'checks': report['checks'], 'actual_products': len(report['products'])}))


if __name__ == '__main__':
    main()
