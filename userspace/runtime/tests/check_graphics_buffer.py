#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Actual compiler/reflection and independent owned buffer-image tests, no GPU."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    out = parser.parse_args().output.resolve()
    if out.exists():
        parser.error('Output evidence directory must be new')
    translation = ROOT / 'build/apple-fragment-buffer-translation-1/report.json'
    compiler_evidence = ROOT / 'build/gfx11-fragment-buffer-tests-4/report.json'
    vertex_evidence = ROOT / 'build/apple-graphics-integration-1/report.json'
    tr, fs, vs = [json.loads(p.read_text()) for p in (translation, compiler_evidence, vertex_evidence)]
    if not all(row['success'] for row in (tr, fs, vs)):
        parser.error('Actual compiler evidence must have succeeded')
    tools = tr['tools']
    for name in ('compiler', 'air_dis', 'spirv_val'):
        if sha(Path(tools[name]['path'])) != tools[name]['sha256']:
            parser.error('Actual explicit compiler/tool bytes changed')
    fixtures = [
        ('userspace/compiler/fixtures/apple-fragment-buffer/fragment-buffer.air', tr['products'][0]['input_sha256']),
        ('userspace/compiler/fixtures/apple-fragment-buffer/fragment-buffer.metallib', tr['products'][1]['input_sha256']),
        ('build/apple-fragment-buffer-translation-1/air/shader.spv', tr['products'][0]['spirv_sha256']),
        ('build/apple-graphics-integration-1/air-vertex/product/shader.bin', vs['products'][0]['code_sha256']),
        ('build/gfx11-fragment-buffer-tests-4/air/shader.bin', fs['products'][0]['code_sha256']),
        ('build/apple-graphics-integration-1/air-fragment/product/shader.bin', vs['products'][1]['code_sha256']),
    ]
    for path, digest in fixtures:
        if sha(ROOT / path) != digest:
            parser.error('Actual fixture changed: ' + path)
    sources = ['userspace/runtime/graphics_shader.hpp', 'userspace/runtime/graphics_shader.cpp',
               'userspace/runtime/graphics_buffer_snapshot.hpp', 'userspace/runtime/graphics_buffer_snapshot.cpp',
               'userspace/runtime/graphics_snapshot.hpp', 'userspace/runtime/graphics_snapshot.cpp',
               'userspace/runtime/graphics_build.hpp', 'userspace/runtime/graphics_build.cpp',
               'userspace/runtime/tests/graphics_buffer_tests.cpp', 'userspace/runtime/tests/check_graphics_buffer.py',
               'src/gfx11_graphics_product.cpp', 'include/hackynavi/gfx11_graphics_product.hpp',
               'src/gfx11_color_target.cpp', 'include/hackynavi/gfx11_color_target.h',
               'userspace/compiler/hackynavi_gfx11_fragment_buffer.h', 'userspace/compiler/hackynavi_gfx11_fragment.h',
               'userspace/compiler/hackynavi_gfx11_ngg.h', 'userspace/metal/include/hackynavi_shader.h',
               'include/hackynavi/native_abi.hpp', 'userspace/transport/iokit_client.hpp',
               'userspace/runtime/compute_context.hpp', 'userspace/runtime/compute_shader.hpp']
    zig = ROOT / '.tools/zig-x86_64-windows-0.17.0/zig.exe'
    report = {'success': False, 'files': {n: sha(ROOT / n) for n in sources},
              'fixtures': dict(fixtures), 'evidence': {str(p.relative_to(ROOT)): sha(p) for p in (translation, compiler_evidence, vertex_evidence)},
              'tools': tools, 'compiler_sha256': sha(zig), 'commands': [], 'gpu_executed': False,
              'metal_objects_created': False, 'context_opened': False,
              'full_pipeline_builder_execution': 'pending actual native graphics library integration'}
    out.mkdir(parents=True)
    environment = os.environ.copy()
    environment['PATH'] = str(Path(tools['compiler']['path']).parent) + os.pathsep + environment.get('PATH', '')
    def run(argv):
        process = subprocess.run([str(v) for v in argv], cwd=ROOT, env=environment, capture_output=True, text=True, timeout=120)
        report['commands'].append({'argv': [str(v) for v in argv], 'exit_code': process.returncode,
                                   'stdout': process.stdout, 'stderr': process.stderr})
        if process.returncode:
            raise RuntimeError(process.stdout + process.stderr)
    try:
        flags = [zig, 'c++', '-std=c++17', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-Iinclude']
        units = ['userspace/runtime/graphics_shader.cpp', 'userspace/runtime/graphics_buffer_snapshot.cpp',
                 'userspace/runtime/graphics_snapshot.cpp', 'src/gfx11_graphics_product.cpp', 'src/gfx11_color_target.cpp',
                 'userspace/runtime/tests/graphics_buffer_tests.cpp']
        for label, extra in [('strict', ['-O2']), ('ubsan', ['-O1', '-g', '-fsanitize=undefined', '-fno-sanitize-recover=all'])]:
            executable = out / (label + '.exe')
            run([*flags, *extra, *units, tools['compiler']['path'], '-o', executable])
            run([executable, *[ROOT / path for path, _ in fixtures], tools['air_dis']['path'], tools['spirv_val']['path']])
        for name in ('graphics_shader.cpp', 'graphics_buffer_snapshot.cpp', 'graphics_build.cpp'):
            run([*flags, '-target', 'x86_64-macos.15.0', '-O2', '-c', 'userspace/runtime/' + name,
                 '-o', out / (Path(name).stem + '.o')])
        report['success'] = True
        report['artifacts'] = {p.name: sha(p) for p in out.iterdir() if p.suffix in ('.exe', '.o')}
    finally:
        report['inputs_unchanged'] = all(sha(ROOT / n) == digest for n, digest in report['files'].items())
        report['success'] &= report['inputs_unchanged']
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'success': report['success'], 'report_sha256': sha(out / 'report.json')}))


if __name__ == '__main__':
    main()
