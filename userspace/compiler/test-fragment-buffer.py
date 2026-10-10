#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real Apple/ACO buffer-fragment compilation and bounded machine mutations.

No backend simulation or GPU execution. Positives compile the actual captured
Apple SPIR-V. Negatives mutate that input and pass actual SPIRV-Tools first.
"""
import argparse
import copy
import hashlib
import importlib.util
import itertools
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--translation', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    compiler, translation, output = args.compiler.resolve(), args.translation.resolve(), args.output.resolve()
    if output.exists():
        parser.error('Evidence directory must be new')
    prior = json.loads((compiler.parent / 'report.json').read_text())
    tr = json.loads((translation / 'report.json').read_text())
    if not prior.get('success') or prior['artifact_sha256'] != sha(compiler) or not tr.get('success'):
        parser.error('Requires actual successful compiler and Apple translation')
    for name, value in prior['source_files'].items():
        if sha(ROOT / name) != value:
            parser.error('Compiler input changed')
    for row in tr['products']:
        base = translation / row['format']
        if sha(base / 'shader.spv') != row['spirv_sha256'] or sha(base / 'reflection.json') != row['reflection_sha256']:
            parser.error('Actual translation bytes changed')
    tool = Path(tr['tools']['spirv_val']['path'])
    if sha(tool) != tr['tools']['spirv_val']['sha256']:
        parser.error('Actual SPIRV-Tools validator changed')
    assembler = tool.with_name('spirv-as.exe')
    legacy_report = ROOT / 'build/gfx11-fragment-tests-4/report.json'
    legacy_record = json.loads(legacy_report.read_text())['tools']['fragment_compiler']
    legacy_compiler = Path(legacy_record['path'])
    if sha(legacy_compiler) != legacy_record['sha256']:
        parser.error('Historical constant-fragment executable changed')
    spec = importlib.util.spec_from_file_location('fragment_buffer_decode', HERE / 'verify-fragment-buffer.py')
    decoder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(decoder)
    preserved = [HERE / name for name in ('fragment.c', 'hackynavi_gfx11_fragment.h', 'build-fragment.py',
                                          'ngg.c', 'compiler.c')]
    # Shared kernel integration proceeds independently in another worker. Only
    # these unchanged compiler profiles are this runner's preservation boundary.
    before = {str(p.relative_to(ROOT)): sha(p) for p in preserved}
    output.mkdir(parents=True)
    report = {'success': False, 'checks': 0, 'compiler_report_sha256': sha(compiler.parent / 'report.json'),
              'translation_report_sha256': sha(translation / 'report.json'), 'compiler_sha256': sha(compiler),
              'runner_sha256': sha(Path(__file__)), 'decoder_sha256': sha(HERE / 'verify-fragment-buffer.py'),
              'assembler_sha256': sha(assembler), 'validator_sha256': sha(tool), 'commands': [], 'products': [],
              'legacy_report_sha256': sha(legacy_report), 'legacy_compiler': legacy_record,
              'negative_inputs': [], 'preserved_inputs': before, 'gpu_executed': False, 'metal_objects_created': False}
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
    try:
        identities = []
        for row in tr['products']:
            product = output / row['format']
            product.mkdir()
            reflection = row['reflection']
            check(reflection['stage'] == 'Fragment' and reflection['entry_point'] == 'hn_buffer_fragment', 'Actual entry/stage')
            bindings = reflection['bindings']
            check(len(bindings) == 1, 'One real binding')
            binding = bindings[0]
            check(binding['kind'] == 'Buffer' and binding['metal_index'] == 0 and
                  binding['descriptor'] == {'set': 0, 'binding': 0, 'count': 1} and
                  binding['access'] == 'ReadOnly' and binding['declared_size'] == 16 and
                  binding['footprint'] == {'static_ranges': [{'offset': 0, 'size': 16}],
                                           'strided_accesses': [], 'has_unbounded_access': False}, 'Actual read-only16B reflection')
            run([tool, '--target-env', 'vulkan1.2', translation / row['format'] / 'shader.spv'])
            run([compiler, translation / row['format'] / 'shader.spv', row['entry'], product])
            code = (product / 'shader.bin').read_bytes()
            metadata = json.loads((product / 'shader.json').read_text())
            decoded = decoder.verify(code, metadata)
            (product / 'decoded.json').write_text(json.dumps(decoded, indent=2) + '\n')
            check(decoded['exported_resource_offsets'] == [0, 4, 8, 12], 'Actual machine component/load provenance')
            identities.append((code, metadata))
            report['products'].append({'input_format': row['format'], 'code_sha256': sha(product / 'shader.bin'),
                                       'metadata_sha256': sha(product / 'shader.json'), 'decoded_sha256': sha(product / 'decoded.json'),
                                       'metadata': metadata})
        check(identities[0] == identities[1], 'Actual AIR and metallib compile identically')
        legacy_output = output / 'legacy-rejects-buffer'
        legacy_output.mkdir()
        run([legacy_compiler, translation / 'air/shader.spv', 'main', legacy_output], 2)
        check(not list(legacy_output.iterdir()), 'Existing constant profile still rejects real buffer shader')
        legacy_spirv = ROOT / 'build/gfx11-fragment-tests-4/white/shader.spv'
        run([tool, '--target-env', 'vulkan1.2', legacy_spirv])
        buffer_output = output / 'buffer-rejects-constant'
        buffer_output.mkdir()
        run([compiler, legacy_spirv, 'main', buffer_output], 2)
        check(not list(buffer_output.iterdir()), 'New buffer profile does not replace load with constants')
        source = (translation / 'air/shader.spvasm').read_text()
        mutations = {
            'binding1': source.replace('OpDecorate %12 Binding 0', 'OpDecorate %12 Binding 1'),
            'set1': source.replace('OpDecorate %12 DescriptorSet 0', 'OpDecorate %12 DescriptorSet 1'),
            'writable': source.replace('               OpDecorate %12 NonWritable\n', ''),
            'volatile': source.replace('OpLoad %uint %19 Aligned 16', 'OpLoad %uint %19 Volatile|Aligned 16'),
            'offset16': source.replace('%uint_3 = OpConstant %uint 3', '%uint_3 = OpConstant %uint 4'),
            'duplicate_component': source.replace('%12 %uint_0 %uint_3', '%12 %uint_0 %uint_2'),
            'rgba_swap': source.replace('%v4float %21 %24 %27 %30', '%v4float %24 %21 %27 %30'),
            'constant_component': source.replace('OpBitcast %float %20', 'OpBitcast %float %uint_0'),
            'dynamic_offset': source.replace('%12 %uint_0 %uint_2', '%12 %uint_0 %20'),
            'arithmetic_color': source.replace('%31 = OpCompositeConstruct', '%double = OpFAdd %float %21 %21\n         %31 = OpCompositeConstruct').replace(
                '%v4float %21 %24 %27 %30', '%v4float %double %24 %27 %30'),
            'output_location1': source.replace('OpDecorate %14 Location 0', 'OpDecorate %14 Location 1'),
            'capability_int64': source.replace('OpCapability Shader', 'OpCapability Shader\n               OpCapability Int64'),
            'conditional_output': source.replace('%float = OpTypeFloat 32', '%bool = OpTypeBool\n      %float = OpTypeFloat 32').replace(
                '               OpStore %14 %31\n',
                '     %swapped = OpCompositeConstruct %v4float %24 %21 %27 %30\n'
                '        %test = OpIEqual %bool %20 %uint_0\n'
                '               OpSelectionMerge %merge None\n'
                '               OpBranchConditional %test %yes %no\n'
                '         %yes = OpLabel\n'
                '               OpStore %14 %31\n'
                '               OpBranch %merge\n'
                '          %no = OpLabel\n'
                '               OpStore %14 %swapped\n'
                '               OpBranch %merge\n'
                '       %merge = OpLabel\n'),
            'second_output': source.replace('%12 %14\n', '%12 %14 %extra\n').replace(
                'OpDecorate %14 Location 0', 'OpDecorate %14 Location 0\n               OpDecorate %extra Location 1').replace(
                '%14 = OpVariable %_ptr_Output_v4float Output', '%14 = OpVariable %_ptr_Output_v4float Output\n      %extra = OpVariable %_ptr_Output_v4float Output').replace(
                'OpStore %14 %31', 'OpStore %14 %31\n               OpStore %extra %31'),
            'second_resource': source.replace('%12 %14\n', '%12 %14 %extra\n').replace(
                'OpDecorate %12 NonWritable', 'OpDecorate %12 NonWritable\n               OpDecorate %extra NonWritable\n               OpDecorate %extra DescriptorSet 0\n               OpDecorate %extra Binding 1').replace(
                '%12 = OpVariable %_ptr_StorageBuffer__struct_10 StorageBuffer', '%12 = OpVariable %_ptr_StorageBuffer__struct_10 StorageBuffer\n      %extra = OpVariable %_ptr_StorageBuffer__struct_10 StorageBuffer'),
        }
        for label, changed in mutations.items():
            check(source != changed, 'Meaningful SPIR-V mutation ' + label)
            directory = output / ('reject-' + label)
            directory.mkdir()
            (directory / 'source.spvasm').write_text(changed)
            spirv = directory / 'shader.spv'
            run([assembler, '--target-env', 'vulkan1.2', directory / 'source.spvasm', '-o', spirv])
            run([tool, '--target-env', 'vulkan1.2', spirv])
            product = directory / 'product'
            product.mkdir()
            result = run([compiler, spirv, 'main', product], 2)
            check(not list(product.iterdir()) and bool(result.stderr), 'Rejected before product publication')
            report['negative_inputs'].append({'label': label, 'spirv_sha256': sha(spirv), 'diagnostic': result.stderr})
        code, metadata = identities[0]
        words = list(struct.unpack('<18I', code))
        # Every single changed instruction/operand/padding bit of this exact
        # product must fail its unchanged export and metadata contract.
        for index in range(len(words)):
            for bit in range(32):
                changed = words.copy()
                changed[index] ^= 1 << bit
                rejects(struct.pack('<18I', *changed), metadata, f'ISA bit mutation {index}:{bit}')
        report['isa_bit_mutations'] = len(words) * 32
        for size in range(len(code)):
            rejects(code[:size], metadata, 'Truncated actual product')
        for label, patch in [('user_sgprs', 0), ('buffer_read_bytes', 32), ('resource_count', 0),
                             ('spi_shader_pgm_rsrc2_ps', 0), ('num_vgprs', 23), ('executable_bytes', 48),
                             ('mem_ordered', 0), ('wave_size', 64), ('color_bits', [0, 0, 0, 0])]:
            changed = copy.deepcopy(metadata)
            changed[label] = patch
            rejects(code, changed, 'Metadata mutation ' + label)
        # This is a provenance checker, not a whole-image digest allowlist:
        # all24 instruction orderings and several different VGPR assignments
        # retain the exact resource-to-component relationship.
        for order in itertools.permutations(range(4)):
            for base in (0, 4, 12, 20):
                changed = [words[0]]
                for component in order:
                    changed += [0xe0500000 | component * 4, 0x80000080 | (base + component) << 8]
                changed += [words[9], words[10], sum((base + component) << (8 * component) for component in range(4)), *words[12:]]
                check(decoder.verify(struct.pack('<18I', *changed), metadata)['success'], 'Valid order/register provenance')
        report['equivalent_machine_layouts'] = 96
        descriptors = []
        for address in (16, 4096, 0xffffffff0, 0x1234567890, (1 << 48) - 16):
            result = run([compiler, '--descriptor', hex(address)])
            descriptor = json.loads(result.stdout)
            check(descriptor == [address & 0xffffffff, address >> 32, 16, 0x30016fac], 'Actual Mesa raw16 descriptor')
            descriptors.append({'address': address, 'descriptor': descriptor})
        for address in ('0', '1', '15', '17', '-16', '0x1000000000000', 'not-a-number', '18446744073709551616'):
            result = run([compiler, '--descriptor', address], 2)
            check(not result.stdout, 'Invalid descriptor has no output')
        report['actual_mesa_descriptors'] = descriptors
        report['preserved_inputs_unchanged'] = all(sha(ROOT / name) == value for name, value in before.items())
        check(report['preserved_inputs_unchanged'], 'Existing compiler/admission sources unchanged')
        report['success'] = True
    finally:
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'success': report['success'], 'checks': report['checks'], 'report_sha256': sha(output / 'report.json')}))


if __name__ == '__main__':
    main()
