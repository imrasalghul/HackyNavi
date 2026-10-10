#!/usr/bin/env python3
"""Compile userspace DML32 and run its calculation tests, without hardware access."""
import argparse
import hashlib
import json
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default=str(ROOT / '.tools/zig-x86_64-windows-0.17.0/zig.exe'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    compiler = pathlib.Path(args.compiler).resolve()
    zig = compiler.name.startswith('zig')
    prefix = [str(compiler)] + (['cc'] if zig else [])
    cxx = [str(compiler), 'c++'] if zig else [str(compiler.with_name('clang++' if compiler.name == 'clang' else 'g++'))]
    out = ROOT / 'build/display-dml'
    out.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((HERE / 'vendor/source-manifest.json').read_text())
    for entry in manifest['files']:
        path = HERE / 'vendor/dml' / entry['path']
        if hashlib.sha256(path.read_bytes()).hexdigest() != entry['sha256']:
            raise RuntimeError(f'Vendor source changed: {path}')
    sources = sorted((HERE / 'vendor/dml').rglob('*.c')) + [HERE / 'dml32_runtime.c', HERE / 'dml32_plan.c', HERE / 'dml32_asic.c']
    dependencies = [ROOT / p for p in ('include/hackynavi/discovery.hpp', 'include/hackynavi/display_bandwidth.hpp',
                    'src/discovery.cpp', 'src/display_bandwidth.cpp', 'include/hackynavi/display_model_input.hpp',
                    'include/hackynavi/display_smu.hpp', 'include/hackynavi/ip_layout.hpp', 'include/hackynavi/rom.hpp',
                    'include/hackynavi/edid.hpp', 'src/display_model_input.cpp', 'src/ip_layout.cpp', 'src/rom.cpp')]
    executable = out / ('calculation.exe' if sys.platform == 'win32' else 'calculation')
    flags = ['-O1', '-g', '-fno-strict-aliasing', '-fno-fast-math',
        '-I' + str(HERE / 'compat'), '-I' + str(HERE / 'vendor/dml'),
        '-I' + str(HERE / 'vendor/dml/calcs'), '-I' + str(HERE), '-I' + str(ROOT / 'include')]
    if args.sanitize:
        flags += ['-fsanitize=address,undefined,float-cast-overflow', '-fno-sanitize-recover=all']
    objects, commands = [], []
    for n, source in enumerate(sources):
        obj = out / f'dml-{n}.o'
        objects.append(str(obj))
        commands.append(prefix + ['-std=c11'] + flags + ['-c', str(source), '-o', str(obj)])
    commands.append(prefix + ['-std=c11'] + flags + objects + [str(HERE / 'tests/calculation.c'), '-o', str(executable)])
    commands.append([str(executable)])
    fixture = out / 'encode-fixture.o'
    commands.append(prefix + ['-std=c11'] + flags + ['-c', str(HERE / 'tests/encode_fixture.c'), '-o', str(fixture)])
    encoding = out / ('encoding.exe' if sys.platform == 'win32' else 'encoding')
    commands.append(cxx + ['-std=c++14', '-Wall', '-Wextra', '-Werror'] + flags + objects + [str(fixture),
        str(HERE / 'dml32_encode.cpp'), str(HERE / 'tests/encoding.cpp'),
        str(ROOT / 'src/discovery.cpp'), str(ROOT / 'src/display_bandwidth.cpp'), '-o', str(encoding)])
    commands.append([str(encoding)])
    profile = out / ('profile.exe' if sys.platform == 'win32' else 'profile')
    commands.append(cxx + ['-std=c++14', '-Wall', '-Wextra', '-Werror'] + flags + objects + [
        str(HERE / 'dml32_profile.cpp'), str(HERE / 'tests/profile.cpp'),
        str(ROOT / 'src/display_model_input.cpp'), str(ROOT / 'src/rom.cpp'),
        str(ROOT / 'src/ip_layout.cpp'), str(ROOT / 'src/discovery.cpp'), '-o', str(profile)])
    commands.append([str(profile)])
    paths = sorted(p for p in HERE.rglob('*') if p.is_file() and '__pycache__' not in p.parts) + dependencies
    report = {'scope': 'CPU DML32 calculations and integer register plan encoding; no GPU or modeset evidence', 'success': False,
        'sources': {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in paths}, 'commands': []}
    try:
        with (out / 'test.log').open('w') as log:
            for cmd in commands:
                result = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, timeout=180)
                report['commands'].append({'argv': cmd, 'exit_code': result.returncode})
                if result.returncode:
                    raise RuntimeError(f'Command failed ({result.returncode}); see {out / "test.log"}')
        report['success'] = True
        report['executable_sha256'] = hashlib.sha256(executable.read_bytes()).hexdigest()
        report['encoding_executable_sha256'] = hashlib.sha256(encoding.read_bytes()).hexdigest()
        report['profile_executable_sha256'] = hashlib.sha256(profile.read_bytes()).hexdigest()
        print((out / 'test.log').read_text())
    finally:
        report['log_sha256'] = hashlib.sha256((out / 'test.log').read_bytes()).hexdigest()
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')

if __name__ == '__main__':
    main()
