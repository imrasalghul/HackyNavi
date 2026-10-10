from pathlib import Path
import hashlib
import json
import struct
import subprocess
import tempfile
import time

from compile import ROOT, compile_shader, digest


def instructions(words):
    offset = 5
    while offset < len(words):
        count = words[offset] >> 16
        if not count or offset + count > len(words):
            raise ValueError('invalid SPIR-V instruction')
        yield offset, words[offset] & 0xffff, count
        offset += count


def main():
    source = ROOT / 'build/metal-compiler/apple-input/translated/shader.spv'
    compiler = ROOT / '.tools/userspace/build-mesa-compiler/hackynavi-compiler/hackynavi-aco.exe'
    original = list(struct.unpack(f'<{source.stat().st_size // 4}I', source.read_bytes()))
    destination = ROOT / 'build/gfx11-compiler'
    destination.mkdir(exist_ok=True)
    report = {'source': str(source), 'source_sha256': digest(source), 'compiler_sha256': digest(compiler), 'checks': [], 'hardware_executed': False}
    with tempfile.TemporaryDirectory(prefix='test-', dir=destination) as temp:
        work = Path(temp)
        baseline = work / 'baseline'
        config = compile_shader(source, baseline)
        ir = (baseline / 'aco.txt').read_text().strip('\x00')
        if not all(token in ir for token in ['buffer_load', 'buffer_store', 's_endpgm']):
            raise AssertionError(f'Executable ACO IR lost the required buffer effects:\n{ir}')
        if config['descriptor_binding_mask'] != 3 or config['scratch_bytes_per_wave'] or config['lds_bytes']:
            raise AssertionError('unexpected fixture resource usage')
        report['checks'].append('Apple AIR-derived SPIR-V emitted GFX11 machine code with buffer load/store/end and binding mask 3')
        for local in [(3, 1, 1), (8, 8, 1), (4, 4, 4)]:
            result = compile_shader(source, work / ('local-' + '-'.join(map(str, local))), local=local)
            if result['workgroup_size'] != list(local):
                raise AssertionError('specialization lost')
        report['checks'].append('Explicit local-size specializations 3x1x1, 8x8x1 and 4x4x4 compile')
        variant = original.copy()
        changed = False
        for offset, opcode, count in instructions(variant):
            if opcode == 43 and count == 4 and variant[offset + 3] == 1:
                variant[offset + 3] = 7
                changed = True
                break
        if not changed:
            raise AssertionError('fixture increment constant not found')
        variant_file = work / 'add-seven.spv'
        variant_file.write_bytes(struct.pack(f'<{len(variant)}I', *variant))
        compile_shader(variant_file, work / 'add-seven')
        if (baseline / 'shader.bin').read_bytes() == (work / 'add-seven/shader.bin').read_bytes():
            raise AssertionError('Changing the actual arithmetic did not change machine code')
        report['checks'].append('Changing the increment from 1 to 7 changes emitted machine code')

        def rejected(name, words=None, **kwargs):
            fixture = source
            if words is not None:
                fixture = work / (name + '.spv')
                fixture.write_bytes(struct.pack(f'<{len(words)}I', *words))
            output = work / name
            try:
                compile_shader(fixture, output, **kwargs)
            except (ValueError, RuntimeError, subprocess.CalledProcessError, FileNotFoundError) as error:
                if output.exists():
                    raise AssertionError('failed compilation published output')
                report['checks'].append({'rejected': name, 'reason': str(error)[:600]})
            else:
                raise AssertionError(name + ' was accepted')

        rejected('bad-header', [0x07230203, 0x00010400, 0, 1, 0])
        rejected('missing-validator', validator=work / 'absent-validator')
        rejected('wrong-entry', entry='missing_entry')
        rejected('invalid-local-size', local=(1024, 2, 1))
        capability = original[:5] + [(2 << 16) | 17, 10] + original[5:]
        rejected('float64-capability', capability)
        binding = original.copy()
        for offset, opcode, count in instructions(binding):
            if opcode == 71 and count == 4 and binding[offset + 2] == 33:
                binding[offset + 3] = 33
                break
        rejected('binding-33', binding)
        group = original.copy()
        for offset, opcode, count in instructions(group):
            if opcode == 71 and count == 4 and group[offset + 2] == 34:
                group[offset + 3] = 1
                break
        rejected('descriptor-set-1', group)
        unknown_spec = original.copy()
        for offset, opcode, count in instructions(unknown_spec):
            if opcode == 71 and count == 4 and unknown_spec[offset + 2] == 1:
                unknown_spec[offset + 3] = 9
                break
        rejected('unknown-specialization', unknown_spec)
        barrier = original.copy()
        scope_id = semantics_id = None
        for offset, opcode, count in instructions(barrier):
            if opcode == 43 and count == 4:
                if barrier[offset + 3] == 1:
                    barrier[offset + 3] = 2
                    scope_id = barrier[offset + 2]
                elif barrier[offset + 3] == 0:
                    semantics_id = barrier[offset + 2]
        if scope_id is None or semantics_id is None:
            raise AssertionError('fixture constants for a real control barrier were not found')
        for offset, opcode, count in instructions(barrier):
            if opcode == 253:
                barrier[offset:offset] = [(4 << 16) | 224, scope_id, scope_id, semantics_id]
                break
        rejected('workgroup-control-barrier', barrier)
        descriptor = json.loads(subprocess.check_output([str(compiler), '--raw-buffer-descriptor', '0x1234000000', '16384'], text=True))
        if descriptor[0] != 0x34000000 or descriptor[1] & 0xffff != 0x12 or descriptor[2] != 16384 or len(descriptor) != 4:
            raise AssertionError('raw descriptor lost the GPU address or byte bounds')
        if subprocess.run([str(compiler), '--raw-buffer-descriptor', '0xffffffffffff', '16384'], capture_output=True).returncode == 0:
            raise AssertionError('raw descriptor accepted a 48-bit address overflow')
        report['checks'].append({'raw_buffer_descriptor': descriptor, 'address_overflow_rejected': True})
        evidence = destination / ('apple-add-one-' + time.strftime('%Y%m%d-%H%M%S'))
        baseline.rename(evidence)
        report['artifact_directory'] = str(evidence)
        report['shader_sha256'] = digest(evidence / 'shader.bin')
        report['config'] = config
    report['success'] = True
    (destination / 'test-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
