#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check actual buffer shaders, independent admission, Mesa descriptors and kernel objects.

CPU-only checks. No GPU, remote host, Metal object or fabricated execution result.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ['src/gfx11_graphics_product.cpp', 'src/gfx11_graphics_draw.cpp',
           'src/gfx11_graphics_barrier.cpp', 'src/gfx11_color_target.cpp']
TESTS = ['gfx11_fragment_buffer', 'gfx11_buffer_draw', 'gfx11_graphics_product', 'gfx11_graphics_draw']
OWNED = SOURCES + ['include/hackynavi/gfx11_graphics_product.hpp', 'include/hackynavi/gfx11_graphics_draw.hpp',
                  'include/hackynavi/gfx11_graphics_barrier.hpp', 'include/hackynavi/gfx11_color_target.h',
                  'include/hackynavi/isolated_compute.hpp', 'tests/fixtures/gfx11_buffer_shader.hpp',
                  'scripts/check-gfx11-fragment-buffer-local.py', 'CMakeLists.txt', 'scripts/build-host.py']
OWNED += ['tests/' + n + '_tests.cpp' for n in TESTS]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--compiler', required=True, type=Path, help='Actual Mesa fragment-buffer oracle executable')
    args = parser.parse_args()
    out, oracle = args.output.resolve(), args.compiler.resolve()
    if out.exists():
        parser.error('Use a new evidence directory')
    prior = json.loads((oracle.parent / 'report.json').read_text())
    if not prior['success'] or prior['artifact_sha256'] != sha(oracle):
        parser.error('Requires successful actual compiler evidence')
    for path, digest in prior['source_files'].items():
        if sha(ROOT / path) != digest:
            parser.error('Actual compiler source changed: ' + path)
    zig = ROOT / '.tools/zig-x86_64-windows-0.17.0/zig.exe'
    out.mkdir(parents=True)
    upstream = ['.research/mesa/src/amd/registers/gfx11.json', '.research/mesa/src/amd/common/ac_descriptors.c']
    report = {'success': False, 'gpu_executed': False, 'remote_executed': False,
              'source_files': {p: sha(ROOT / p) for p in OWNED + upstream},
              'compiler_sha256': sha(zig), 'mesa_oracle_sha256': sha(oracle),
              'mesa_oracle_report_sha256': sha(oracle.parent / 'report.json'), 'commands': []}

    def run(command):
        r = subprocess.run([str(x) for x in command], cwd=ROOT, capture_output=True, text=True, timeout=120)
        report['commands'].append({'argv': [str(x) for x in command], 'exit_code': r.returncode,
                                   'stdout': r.stdout, 'stderr': r.stderr})
        if r.returncode:
            raise RuntimeError(r.stdout + r.stderr)
        return r

    try:
        header = (ROOT / 'tests/fixtures/gfx11_buffer_shader.hpp').read_text()
        products = [('vertex', 'build/apple-graphics-integration-1/air-vertex/product/shader.bin'),
                    ('fragment', 'build/gfx11-fragment-buffer-tests-1/air/shader.bin')]
        report['fixture_provenance'] = {}
        for name, path in products:
            body = re.search(r'uint32_t ' + name + r'\[\] = \{([^}]+)', header).group(1)
            words = [int(x, 16) for x in re.findall(r'0x([a-f0-9]+)u', body)]
            assert struct.pack('<' + str(len(words)) + 'I', *words) == (ROOT / path).read_bytes()
            report['fixture_provenance'][path] = sha(ROOT / path)
        registers = json.loads((ROOT / upstream[0]).read_text())['register_mappings']
        for i in range(4):
            reg = [r for r in registers if r['name'] == 'SPI_SHADER_USER_DATA_PS_' + str(i)]
            assert len(reg) == 1 and reg[0]['map'] == {'at': 0xb030 + 4*i, 'to': 'mm'}
        report['ps_user_data_addresses'] = [0xb030 + 4*i for i in range(4)]
        for mode, extras in [('strict', []), ('ubsan', ['-fsanitize=undefined', '-fno-sanitize-recover=all'])]:
            for test in TESTS:
                exe = out / (test + '-' + mode + '.exe')
                sources = SOURCES if 'draw' in test else SOURCES[:1]
                run([zig, 'c++', '-std=c++11', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                     '-fno-exceptions', '-fno-rtti', '-Iinclude', *extras, *sources, 'tests/' + test + '_tests.cpp', '-o', exe])
                run([exe, out / 'descriptors.txt'] if test == 'gfx11_buffer_draw' and mode == 'strict' else [exe])
        comparisons = []
        for line in (out / 'descriptors.txt').read_text().splitlines():
            address, *descriptor = [int(x) for x in line.split()]
            actual = json.loads(run([oracle, '--descriptor', str(address)]).stdout)
            assert actual == descriptor
            comparisons.append({'address': address, 'descriptor': descriptor})
        assert len(comparisons) == 5
        report['actual_mesa_descriptors'] = comparisons
        flags = [zig, 'c++', '-target', 'x86_64-macos.15.0', '-std=c++11', '-mkernel', '-fapple-kext',
                 '-DKERNEL', '-DKERNEL_EXTENSION', '-nostdinc', '-nostdlibinc', '-I', '.research/MacKernelSDK/Headers',
                 '-Iinclude', '-fno-exceptions', '-fno-rtti', '-fno-builtin', '-fno-common', '-fno-stack-protector',
                 '-mno-red-zone', '-mno-mmx', '-mno-sse', '-mno-sse2', '-Wall', '-Wextra', '-Werror',
                 '-Wframe-larger-than=2048', '-O2']
        report['kernel_objects'] = {}
        for source in SOURCES[:2]:
            stem = Path(source).stem
            def deps(path):
                return {p: sha(ROOT / p) for p in path.read_text().replace('\\\n', ' ').split(':', 1)[1].split()}
            run([*flags, '-M', '-MT', stem + '.o', '-MF', out / (stem + '-before.d'), source])
            before = deps(out / (stem + '-before.d'))
            run([*flags, '-MD', '-MT', stem + '.o', '-MF', out / (stem + '-compiled.d'), '-c', source, '-o', out / (stem + '.o')])
            assert before == deps(out / (stem + '-compiled.d'))
            report['kernel_objects'][source] = {'sha256': sha(out / (stem + '.o')), 'dependencies': before}
        report['success'] = True
    finally:
        report['inputs_unchanged'] = all(sha(ROOT / p) == h for p, h in report['source_files'].items())
        report['success'] &= report['inputs_unchanged']
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'success': report['success'], 'report_sha256': sha(out / 'report.json')}))
    if not report['success']:
        raise RuntimeError('Validation failed')


if __name__ == '__main__':
    main()
