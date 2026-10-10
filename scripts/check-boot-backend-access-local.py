#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Actual callback-gate tests and strict real-kernel-header compile, no GPU/SSH."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OWNED = ['include/hackynavi/boot_backend_access.hpp', 'tests/boot_backend_access_tests.cpp',
         'kext/HackyNavi/HackyNaviBootBackendCapture.hpp', 'kext/HackyNavi/HackyNaviBootBackendCapture.cpp',
         'docs/boot-backend-access.md', 'scripts/check-boot-backend-access-local.py']
SOURCE = ['dcn32_hwseq.c', 'dcn32_optc.c', 'dcn32_mpc.c', 'dcn32_dio_link_encoder.c',
          'dcn32_dio_stream_encoder.c', 'dcn31_dio_link_encoder.c', 'dcn10_optc.c', 'dcn10_mpc.c',
          'dcn10_link_encoder.c', 'dcn10_hwseq.c', 'dcn32_resource.c', 'amdgpu_dm.c', 'amdgpu_dm_dmub.c']
NATIVE = 'kext/HackyNavi/HackyNaviBootBackendCapture.cpp'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    out = args.output.resolve()
    if out.exists():
        ap.error('Use a fresh evidence directory')
    out.mkdir(parents=True)
    bound = OWNED + ['include/hackynavi/boot_display_backend.hpp', 'src/boot_display_backend.cpp',
                     'kext/HackyNavi/HackyNaviFunctionGroup.cpp', 'kext/HackyNavi/HackyNaviGARTController.cpp',
                     'kext/HackyNavi/HackyNaviEngineController.cpp'] + ['.research/linux/' + x for x in SOURCE]
    bound += ['build/boot-backend-access-audit/' + x for x in ('dc.c', 'dcn31_optc.c', 'dcn20_link_encoder.c', 'source-retrieval.json')]
    zig = ROOT / '.tools/zig-x86_64-windows-0.17.0/zig.exe'
    report = {'success': False, 'source_files': {p: sha(ROOT / p) for p in bound},
              'compiler_sha256': sha(zig), 'commands': [], 'gpu_executed': False, 'remote_executed': False,
              'scope': 'Production one-shot read gate tested with numeric callback records; actual PCI/MMIO never executed'}

    def run(command):
        result = subprocess.run([str(x) for x in command], cwd=ROOT, capture_output=True, text=True, timeout=120)
        report['commands'].append({'args': [str(x) for x in command], 'exit_code': result.returncode,
                                   'stdout': result.stdout, 'stderr': result.stderr})
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    try:
        for name, extra in [('strict', []), ('ubsan', ['-fsanitize=undefined', '-fno-sanitize-recover=all'])]:
            exe = out / (name + '.exe')
            run([zig, 'c++', '-std=c++11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-Iinclude',
                 *extra, 'tests/boot_backend_access_tests.cpp', '-o', exe])
            run([exe])
        flags = [zig, 'c++', '-target', 'x86_64-macos.15.0', '-std=c++14', '-mkernel', '-fapple-kext',
                 '-DKERNEL', '-DKERNEL_EXTENSION', '-nostdinc', '-nostdlibinc', '-I', '.research/MacKernelSDK/Headers',
                 '-Iinclude', '-fno-exceptions', '-fno-rtti', '-fno-builtin', '-fno-common', '-fno-stack-protector',
                 '-mno-red-zone', '-mno-mmx', '-mno-sse', '-mno-sse2', '-Wall', '-Wextra', '-Werror',
                 '-Wno-unused-parameter', '-Wframe-larger-than=2048', '-O2', '-fstack-usage']
        run([*flags, '-M', '-MT', 'native.o', '-MF', out / 'before.d', NATIVE])

        def deps(path):
            return {p: sha(ROOT / p) for p in path.read_text().replace('\\\n', ' ').split(':', 1)[1].split()}

        report['kernel_dependencies'] = deps(out / 'before.d')
        run([*flags, '-MD', '-MT', 'native.o', '-MF', out / 'compiled.d', '-c', NATIVE, '-o', out / 'native.o'])
        assert report['kernel_dependencies'] == deps(out / 'compiled.d')
        report['artifacts'] = {p.name: sha(p) for p in out.iterdir() if p.suffix in ('.exe', '.o', '.su')}
        report['success'] = True
    finally:
        report['inputs_unchanged'] = all(sha(ROOT / p) == h for p, h in report['source_files'].items())
        report['success'] &= report['inputs_unchanged']
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    if not report['success']:
        raise RuntimeError('Focused validation failed')
    freeze = {'source_files': report['source_files'], 'report_sha256': sha(out / 'report.json'),
              'kernel_dependencies': report['kernel_dependencies'], 'no_target_or_gpu_execution': True}
    (out / 'source-freeze.json').write_text(json.dumps(freeze, indent=2) + '\n')
    print(json.dumps({'freeze_sha256': sha(out / 'source-freeze.json'), 'report_sha256': sha(out / 'report.json')}))


if __name__ == '__main__':
    main()
