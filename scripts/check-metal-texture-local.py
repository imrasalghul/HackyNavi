#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate actual texture CPU helpers locally; no Metal objects or GPU calls."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [
    'userspace/metal/native/HNMetalTexture.hpp',
    'userspace/metal/native/HNMetalTexture.mm',
    'userspace/runtime/texture_region.hpp',
    'userspace/runtime/texture_region.cpp',
    'userspace/runtime/tests/texture_region_tests.cpp',
    'docs/metal-texture-owner.md',
    'scripts/check-metal-texture-local.py',
]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        parser.error('Use a new output directory; earlier evidence is immutable')
    sdk = ROOT / 'build/metal-texture-sdk-capture'
    sdk_report = json.loads((sdk / 'report.json').read_text())
    for name, record in sdk_report['headers'].items():
        if sha(sdk / name) != record['sha256']:
            raise RuntimeError('Captured actual SDK header changed: ' + name)
    zig = ROOT / '.tools/zig-x86_64-windows-0.17.0/zig.exe'
    report = {'success': False, 'gpu_executed': False, 'remote_executed': False,
              'metal_objects_instantiated': False, 'objc_compiled': False,
              'source_files': {p: sha(ROOT / p) for p in SOURCES},
              'compiler_sha256': sha(zig), 'commands': [],
              'sdk_evidence': {str((sdk / name).relative_to(ROOT)): record['sha256']
                               for name, record in sdk_report['headers'].items()},
              'dependencies': {'include/hackynavi/gfx11_color_target.h':
                               sha(ROOT / 'include/hackynavi/gfx11_color_target.h')}}
    output.mkdir(parents=True)

    def run(args):
        result = subprocess.run([str(x) for x in args], cwd=ROOT, text=True,
                                capture_output=True, timeout=120)
        report['commands'].append({'args': [str(x) for x in args], 'exit_code': result.returncode,
                                   'stdout': result.stdout, 'stderr': result.stderr})
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    try:
        common = [zig, 'c++', '-std=c++14', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-Iinclude']
        core = ROOT / 'userspace/runtime/texture_region.cpp'
        tests = ROOT / 'userspace/runtime/tests/texture_region_tests.cpp'
        for name, extra in [('strict', []), ('ubsan', ['-fsanitize=undefined', '-fno-sanitize-recover=all'])]:
            executable = output / (name + '.exe')
            run([*common, *extra, core, tests, '-o', executable])
            run([executable])
        run([*common, '-target', 'x86_64-macos.15.0', '-ffreestanding', '-fno-exceptions', '-fno-rtti',
             '-c', core, '-o', output / 'texture-region-darwin.o'])
        report['artifacts'] = {p.name: sha(p) for p in output.iterdir() if p.suffix in ('.exe', '.o')}
        report['success'] = True
    finally:
        report['inputs_unchanged'] = all(sha(ROOT / p) == h for p, h in report['source_files'].items())
        report['dependencies_unchanged'] = all(sha(ROOT / p) == h for p, h in report['dependencies'].items())
        report['success'] &= report['inputs_unchanged'] and report['dependencies_unchanged']
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    if not report['success']:
        raise RuntimeError('Texture local validation failed')
    freeze = {'source_files': report['source_files'], 'report_sha256': sha(output / 'report.json'),
              'validation': 'Actual CPU helper strict/UBSan tests and Darwin cross-object only; Objective-C SDK compile pending root native lane.'}
    (output / 'source-freeze.json').write_text(json.dumps(freeze, indent=2) + '\n')
    print(json.dumps({'report': str(output / 'report.json'), 'sha256': sha(output / 'report.json'),
                      'freeze_sha256': sha(output / 'source-freeze.json')}))


if __name__ == '__main__':
    main()
