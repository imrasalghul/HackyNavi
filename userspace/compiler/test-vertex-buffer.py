#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Test genuine Apple vertex-buffer translation with real Mesa and SPIRV-Tools.

This runner compiles CPU artifacts and checks machine provenance. It does not
execute shader instructions or claim a completed draw or Metal integration.
"""
import argparse
import copy
import ctypes as c
import hashlib
import importlib.util
import itertools
import json
import os
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def pe_exports(path):
    raw = path.read_bytes()
    pe = struct.unpack_from('<I', raw, 60)[0]
    if raw[:2] != b'MZ' or raw[pe:pe + 4] != b'PE\0\0':
        raise ValueError('Not an actual PE image')
    machine, count = struct.unpack_from('<HH', raw, pe + 4)
    optional_size = struct.unpack_from('<H', raw, pe + 20)[0]
    optional = pe + 24
    if machine != 0x8664 or struct.unpack_from('<H', raw, optional)[0] != 0x20b:
        raise ValueError('Expected PE32+ x86-64')
    sections = [struct.unpack_from('<8sIIII', raw, optional + optional_size + i * 40) for i in range(count)]
    def offset(rva):
        for _, virtual_size, virtual, raw_size, raw_start in sections:
            if virtual <= rva < virtual + min(virtual_size, raw_size):
                return raw_start + rva - virtual
        raise ValueError('RVA has no file-backed section')
    table = offset(struct.unpack_from('<I', raw, optional + 112)[0])
    functions, names = struct.unpack_from('<II', raw, table + 20)
    name_table = offset(struct.unpack_from('<I', raw, table + 32)[0])
    result = []
    for i in range(names):
        start = offset(struct.unpack_from('<I', raw, name_table + i * 4)[0])
        result.append(raw[start:raw.index(b'\0', start)].decode('ascii'))
    if functions != names:
        raise ValueError('Unnamed/ordinal-only ABI export')
    return sorted(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--translation', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    compiler, translation, output = args.compiler.resolve(), args.translation.resolve(), args.output.resolve()
    shared = compiler.with_suffix('.dll')
    if output.exists():
        parser.error('Evidence directory must be new')
    prior = json.loads((compiler.parent / 'report.json').read_text())
    tr = json.loads((translation / 'report.json').read_text())
    if (not prior.get('success') or prior['artifact_sha256'] != sha(compiler) or
            prior['shared_artifact_sha256'] != sha(shared) or not tr.get('success')):
        parser.error('Requires frozen actual compiler and Apple translation')
    for name, value in prior['source_files'].items():
        if sha(ROOT / name) != value:
            parser.error('Compiler source changed')
    for name, value in prior['dependencies'].items():
        if sha(ROOT / name) != value['sha256']:
            parser.error('Compiler dependency changed')
    for row in tr['products']:
        base = translation / row['format']
        for filename, key in [('shader.spv', 'spirv_sha256'), ('reflection.json', 'reflection_sha256'),
                              ('shader.spvasm', 'disassembly_sha256')]:
            if sha(base / filename) != row[key]:
                parser.error('Actual translation bytes changed')
    validator = Path(tr['tools']['spirv_val']['path'])
    if sha(validator) != tr['tools']['spirv_val']['sha256']:
        parser.error('Actual validator changed')
    assembler = validator.with_name('spirv-as.exe')
    legacy_report = ROOT / 'build/gfx11-ngg-tests-3/report.json'
    legacy_record = json.loads(legacy_report.read_text())['tools']['ngg_compiler']
    legacy = Path(legacy_record['path'])
    if sha(legacy) != legacy_record['sha256']:
        parser.error('Historical NGG compiler changed')
    spec = importlib.util.spec_from_file_location('vertex_buffer_decode', HERE / 'verify-vertex-buffer.py')
    decoder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(decoder)
    preserved = ['compiler.c', 'ngg.c', 'hackynavi_gfx11_ngg.h', 'build-ngg.py', 'verify-ngg.py',
                 'fragment.c', 'hackynavi_gfx11_fragment.h', 'build-fragment.py',
                 'fragment-buffer.c', 'hackynavi_gfx11_fragment_buffer.h', 'verify-fragment-buffer.py']
    before = {str((HERE / name).relative_to(ROOT)): sha(HERE / name) for name in preserved}
    output.mkdir(parents=True)
    report = {'success': False, 'checks': 0, 'compiler_report_sha256': sha(compiler.parent / 'report.json'),
              'translation_report_sha256': sha(translation / 'report.json'), 'compiler_sha256': sha(compiler),
              'shared_compiler_sha256': sha(shared), 'runner_sha256': sha(Path(__file__)),
              'decoder_sha256': sha(HERE / 'verify-vertex-buffer.py'), 'assembler_sha256': sha(assembler),
              'validator_sha256': sha(validator), 'commands': [], 'products': [], 'negative_inputs': [],
              'equivalent_spirv': [], 'preserved_inputs': before, 'legacy_compiler': legacy_record,
              'legacy_report_sha256': sha(legacy_report), 'gpu_executed': False, 'metal_objects_created': False}
    def check(value, label):
        report['checks'] += 1
        if not value:
            raise AssertionError(label)
    def run(argv, expected=0):
        result = subprocess.run([str(x) for x in argv], capture_output=True, text=True, timeout=30)
        report['commands'].append({'argv': [str(x) for x in argv], 'exit_code': result.returncode,
                                   'stdout': result.stdout, 'stderr': result.stderr})
        check(result.returncode == expected, 'Command result: ' + result.stderr)
        return result
    def rejects(code, metadata, label):
        try:
            decoder.verify(code, metadata)
        except ValueError:
            check(True, label)
        else:
            check(False, label)
    lib = c.CDLL(str(shared))
    compile_file = lib.hn_gfx11_vertex_buffer_compile_validated_file
    compile_file.argtypes, compile_file.restype = [c.c_char_p] * 3, c.c_int
    descriptor = lib.hn_gfx11_vertex_buffer_descriptor
    descriptor.argtypes, descriptor.restype = [c.c_uint64, c.POINTER(c.c_uint32)], c.c_int
    try:
        exports = pe_exports(shared)
        check(exports == ['hn_gfx11_vertex_buffer_compile_validated_file', 'hn_gfx11_vertex_buffer_descriptor'],
              'Exactly two named C ABI exports; no Mesa internals exposed')
        report['shared_exports'] = exports
        identities = []
        for row in tr['products']:
            reflection = row['reflection']
            check(reflection['stage'] == 'Vertex' and reflection['entry_point'] == 'hn_buffer_vertex', 'Actual entry/stage')
            check(len(reflection['bindings']) == 1, 'One actual binding')
            binding = reflection['bindings'][0]
            check(binding['kind'] == 'Buffer' and binding['metal_index'] == 0 and
                  binding['descriptor'] == {'set': 0, 'binding': 0, 'count': 1} and
                  binding['access'] == 'ReadOnly' and binding['declared_size'] == 16 and
                  binding['extent'] == 'Unbounded' and binding['footprint'] == {
                      'static_ranges': [], 'strided_accesses': [
                          {'base_offset': n, 'access_size': 4, 'terms': [{'source': 'VertexIndex', 'stride': 16}]}
                          for n in (0, 4, 8, 12)], 'has_unbounded_access': False},
                  'Actual reflection is a symbolic indexed access, not a fabricated 48-byte declaration')
            spirv = translation / row['format'] / 'shader.spv'
            run([validator, '--target-env', 'vulkan1.2', spirv])
            for route in ('cli', 'cabi'):
                product = output / (row['format'] + '-' + route)
                product.mkdir()
                if route == 'cli':
                    run([compiler, spirv, row['entry'], product])
                else:
                    check(compile_file(os.fsencode(spirv), row['entry'].encode(), os.fsencode(product)) == 0,
                          'Actual shared C ABI compilation')
                code = (product / 'shader.bin').read_bytes()
                metadata = json.loads((product / 'shader.json').read_text())
                decoded = decoder.verify(code, metadata)
                (product / 'decoded.json').write_text(json.dumps(decoded, indent=2) + '\n')
                check(decoded['required_raw_descriptor_bytes'] == 48 and
                      decoded['required_draw']['first_vertex'] == 0 and decoded['required_draw']['vertices'] == 3,
                      '48-byte range is conditional on trusted draw vertices 0..2')
                identities.append((code, metadata))
                report['products'].append({'input_format': row['format'], 'route': route,
                    'code_sha256': sha(product / 'shader.bin'), 'metadata_sha256': sha(product / 'shader.json'),
                    'decoded_sha256': sha(product / 'decoded.json'), 'metadata': metadata})
        check(all(x == identities[0] for x in identities), 'AIR/metallib and CLI/C ABI products agree')
        for label, exe, spirv, entry in [
            ('legacy-rejects-buffer', legacy, translation / 'air/shader.spv', 'main'),
            ('buffer-rejects-constant', compiler, ROOT / 'build/gfx11-ngg-tests-3/triangle/shader.spv', 'main'),
            ('wrong-entry', compiler, translation / 'air/shader.spv', 'missing')]:
            product = output / label
            product.mkdir()
            run([validator, '--target-env', 'vulkan1.2', spirv])
            run([exe, spirv, entry, product], 2)
            check(not list(product.iterdir()), 'Rejected profile/entry publishes no shader')
        source = (translation / 'air/shader.spvasm').read_text()
        mutations = {
            'binding1': source.replace('OpDecorate %18 Binding 0', 'OpDecorate %18 Binding 1'),
            'set1': source.replace('OpDecorate %18 DescriptorSet 0', 'OpDecorate %18 DescriptorSet 1'),
            'writable': source.replace('               OpDecorate %18 NonWritable\n', ''),
            'volatile': source.replace('OpLoad %uint %30 Aligned 16', 'OpLoad %uint %30 Volatile|Aligned 16'),
            'coherent': source.replace('OpDecorate %18 NonWritable', 'OpDecorate %18 NonWritable\n               OpDecorate %18 Coherent'),
            'stride32': source.replace('%uint_4 = OpConstant %uint 4', '%uint_4 = OpConstant %uint 8'),
            'component16': source.replace('%uint_3 = OpConstant %uint 3', '%uint_3 = OpConstant %uint 4'),
            'duplicate_component': source.replace('%uint %uint_3 %46', '%uint %uint_2 %46'),
            'xy_swap': source.replace('%v4float %32 %38 %44 %50', '%v4float %38 %32 %44 %50'),
            'constant_component': source.replace('OpBitcast %float %31', 'OpBitcast %float %uint_0'),
            'fixed_vertex': source.replace('OpLoad %uint %gl_VertexIndex', 'OpCopyObject %uint %uint_0'),
            'instance_index': source.replace('BuiltIn VertexIndex', 'BuiltIn InstanceIndex'),
            'vertex_attribute': source.replace('BuiltIn VertexIndex', 'Location 0'),
            'vertex_bias1': source.replace('%25 = OpLoad %uint %gl_VertexIndex',
                '%vertex = OpLoad %uint %gl_VertexIndex\n         %25 = OpIAdd %uint %vertex %uint_1'),
            'dynamic_offset': source.replace('%uint %uint_2 %40', '%uint %31 %40'),
            'arithmetic_position': source.replace('%51 = OpCompositeConstruct',
                '%double = OpFAdd %float %32 %32\n         %51 = OpCompositeConstruct').replace(
                '%v4float %32 %38 %44 %50', '%v4float %double %38 %44 %50'),
            'missing_position': source.replace('               OpStore %gl_Position %53\n', ''),
            'specialized_stride': source.replace('%uint_4 = OpConstant %uint 4', '%uint_4 = OpSpecConstant %uint 4'),
            'residual_u64': source.replace('%uint_0 = OpConstant %uint 0',
                '%wide = OpConstant %ulong 1099511627776\n     %uint_0 = OpConstant %uint 0').replace(
                '%27 = OpUConvert %uint %26', '%wdiv = OpUDiv %ulong %wide %26\n         %27 = OpUConvert %uint %wdiv'),
            'conditional_position': source.replace('%float = OpTypeFloat 32',
                '%bool = OpTypeBool\n      %float = OpTypeFloat 32').replace(
                '               OpStore %gl_Position %53\n',
                '     %swapped = OpCompositeConstruct %v4float %38 %32 %44 %50\n'
                '        %test = OpIEqual %bool %25 %uint_0\n'
                '               OpSelectionMerge %merge None\n'
                '               OpBranchConditional %test %yes %no\n'
                '         %yes = OpLabel\n               OpStore %gl_Position %53\n               OpBranch %merge\n'
                '          %no = OpLabel\n               OpStore %gl_Position %swapped\n               OpBranch %merge\n'
                '       %merge = OpLabel\n'),
            'second_output': source.replace('%18 %gl_Position\n', '%18 %gl_Position %extra\n').replace(
                'OpDecorate %gl_Position BuiltIn Position',
                'OpDecorate %gl_Position BuiltIn Position\n               OpDecorate %extra Location 0').replace(
                '%gl_Position = OpVariable %_ptr_Output_v4float Output',
                '%gl_Position = OpVariable %_ptr_Output_v4float Output\n      %extra = OpVariable %_ptr_Output_v4float Output').replace(
                'OpStore %gl_Position %53', 'OpStore %gl_Position %53\n               OpStore %extra %53'),
            'second_resource': source.replace('%18 %gl_Position\n', '%18 %gl_Position %extra\n').replace(
                'OpDecorate %18 NonWritable', 'OpDecorate %18 NonWritable\n               OpDecorate %extra NonWritable\n'
                '               OpDecorate %extra DescriptorSet 0\n               OpDecorate %extra Binding 1').replace(
                '%18 = OpVariable %_ptr_StorageBuffer__struct_16 StorageBuffer',
                '%18 = OpVariable %_ptr_StorageBuffer__struct_16 StorageBuffer\n      %extra = OpVariable %_ptr_StorageBuffer__struct_16 StorageBuffer'),
        }
        for label, changed in mutations.items():
            check(source != changed, 'Meaningful semantic mutation ' + label)
            directory = output / ('reject-' + label)
            directory.mkdir()
            (directory / 'source.spvasm').write_text(changed)
            spirv = directory / 'shader.spv'
            run([assembler, '--target-env', 'vulkan1.2', directory / 'source.spvasm', '-o', spirv])
            run([validator, '--target-env', 'vulkan1.2', spirv])
            product = directory / 'product'
            product.mkdir()
            result = run([compiler, spirv, 'main', product], 2)
            check(not list(product.iterdir()) and bool(result.stderr), 'Semantic rejection before product publication')
            report['negative_inputs'].append({'label': label, 'spirv_sha256': sha(spirv), 'diagnostic': result.stderr})
        equivalent = source
        for number in (27, 33, 39, 45):
            equivalent = equivalent.replace('OpIMul %uint %' + str(number) + ' %uint_4',
                'OpShiftLeftLogical %uint %' + str(number) + ' %uint_2')
        directory = output / 'equivalent-shift-address'
        directory.mkdir()
        (directory / 'source.spvasm').write_text(equivalent)
        run([assembler, '--target-env', 'vulkan1.2', directory / 'source.spvasm', '-o', directory / 'shader.spv'])
        run([validator, '--target-env', 'vulkan1.2', directory / 'shader.spv'])
        product = directory / 'product'
        product.mkdir()
        run([compiler, directory / 'shader.spv', 'main', product])
        check(((product / 'shader.bin').read_bytes(), json.loads((product / 'shader.json').read_text())) == identities[0],
              'Equivalent SPIR-V address arithmetic uses actual Mesa optimization')
        report['equivalent_spirv'].append({'label': 'shift-address', 'spirv_sha256': sha(directory / 'shader.spv')})
        code, metadata = identities[0]
        words = list(struct.unpack('<31I', code))
        for index in range(len(words)):
            for bit in range(32):
                changed = words.copy()
                changed[index] ^= 1 << bit
                rejects(struct.pack('<31I', *changed), metadata, f'ISA bit mutation {index}:{bit}')
        report['isa_bit_mutations'] = len(words) * 32
        for size in range(len(code)):
            rejects(code[:size], metadata, 'Truncated product')
        rejects(code + b'\0\0\0\0', metadata, 'Trailing code')
        for label, patch in [('user_sgprs', 4), ('buffer_bytes', 16), ('resource_count', 0),
                             ('spi_shader_pgm_rsrc2_gs', 8), ('spi_shader_pgm_rsrc1_gs', 0),
                             ('num_vgprs', 25), ('num_sgprs', 12), ('executable_bytes', 100),
                             ('mem_ordered', 0), ('wave_size', 64), ('required_vertex_count', 4),
                             ('required_first_vertex', 1), ('base_vertex_sgpr', 8), ('vertex_stride', 32),
                             ('component_offsets', [0, 8, 4, 12]), ('parameter_exports', 1)]:
            changed = copy.deepcopy(metadata)
            changed[label] = patch
            rejects(code, changed, 'Metadata mutation ' + label)
        layouts = 0
        for order in itertools.permutations(range(4)):
            for address, destinations in [(6, [1, 2, 3, 4]), (8, [20, 21, 22, 23]),
                                          (12, [1, 2, 3, 4]), (23, [16, 17, 18, 19])]:
                changed = words.copy()
                changed[6] = 0xd6470000 | address
                for i, component in enumerate(order):
                    changed[9 + i * 2] = 0xe0500000 | component * 4
                    changed[10 + i * 2] = 0x80420000 | (destinations[component] << 8) | address
                changed[19] = sum(destinations[i] << (8 * i) for i in range(4))
                check(decoder.verify(struct.pack('<31I', *changed), metadata)['success'],
                      'Equivalent register allocation and component-load ordering')
                layouts += 1
        report['equivalent_machine_layouts'] = layouts
        records = []
        for address in (16, 4096, 0xfffffff0, 0x100000000, 0x1234567890, (1 << 48) - 48):
            actual = (c.c_uint32 * 4)(0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef)
            check(descriptor(address, actual) == 0, 'Actual Mesa descriptor C ABI')
            expected = [address & 0xffffffff, address >> 32, 48, 0x30016fac]
            check(list(actual) == expected, 'Actual raw48 descriptor words')
            result = run([compiler, '--descriptor', hex(address)])
            check(json.loads(result.stdout) == expected, 'Actual descriptor CLI agrees')
            records.append({'address': address, 'descriptor': expected})
        for address in (0, 1, 15, 17, (1 << 48) - 32, (1 << 48) - 16, 1 << 48, (1 << 64) - 1):
            actual = (c.c_uint32 * 4)(1, 2, 3, 4)
            check(descriptor(address, actual) == 2 and list(actual) == [1, 2, 3, 4],
                  'Invalid descriptor preserves output')
        check(descriptor(16, None) == 2, 'Null descriptor pointer')
        for address in ('0', '17', '-16', '0x1000000000000', 'not-a-number', '18446744073709551616'):
            check(not run([compiler, '--descriptor', address], 2).stdout, 'Invalid descriptor CLI has no output')
        report['actual_mesa_descriptors'] = records
        valid = os.fsencode(translation / 'air/shader.spv')
        private = output / 'invalid-cabi'
        private.mkdir()
        for arguments in [(None, b'main', os.fsencode(private)), (valid, None, os.fsencode(private)),
                          (valid, b'main', None), (valid, b'', os.fsencode(private)),
                          (valid, b'a' * 256, os.fsencode(private))]:
            check(compile_file(*arguments) == 2, 'Invalid C ABI input')
        for label, raw in [('short', b'\0' * 19), ('unaligned', b'\0' * 21), ('magic', b'\0' * 20),
                           ('zero_instruction', struct.pack('<6I', 0x07230203, 0x00010400, 0, 1, 0, 0)),
                           ('overrun_instruction', struct.pack('<6I', 0x07230203, 0x00010400, 0, 1, 0, 0xffff0011))]:
            path = output / (label + '.spv')
            path.write_bytes(raw)
            check(compile_file(os.fsencode(path), b'main', os.fsencode(private)) == 2,
                  'C ABI preliminary file framing ' + label)
        check(not list(private.iterdir()), 'Rejected C ABI inputs publish no shader')
        report['preserved_inputs_unchanged'] = all(sha(ROOT / name) == value for name, value in before.items())
        check(report['preserved_inputs_unchanged'], 'Previous profiles remain unchanged')
        check(sha(compiler) == report['compiler_sha256'] and sha(shared) == report['shared_compiler_sha256'],
              'Tested actual compiler artifacts remain unchanged')
        report['success'] = True
    finally:
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'success': report['success'], 'checks': report['checks'], 'report_sha256': sha(output / 'report.json')}))


if __name__ == '__main__':
    main()
