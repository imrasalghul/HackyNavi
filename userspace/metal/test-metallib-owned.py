#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Local actual-library owned C ABI and compiler regression checks; no GPU/SSH."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/metal-metallib-owned-local')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise RuntimeError('Use an empty output directory to preserve existing evidence')
    local = ROOT / '.tools/userspace'
    cargo = local / 'cargo/bin/cargo.exe'
    llvm = local / 'src/clang+llvm-20.1.8-x86_64-pc-windows-msvc/bin'
    gcc = local / 'rustup/toolchains/1.90.0-x86_64-pc-windows-gnu/lib/rustlib/x86_64-pc-windows-gnu/bin/self-contained'
    env = os.environ.copy()
    env.update(RUSTUP_HOME=str(local / 'rustup'), CARGO_HOME=str(local / 'cargo'),
               CARGO_TARGET_DIR=str(ROOT / 'build/metal-compiler'),
               METAL2VULKAN_LLVM_DIS=str(llvm / 'llvm-dis.exe'),
               METAL2VULKAN_SPIRV_VAL=str(local / 'build-spirv-tools/tools/spirv-val.exe'))
    env['PATH'] = os.pathsep.join([str(cargo.parent), str(gcc), env.get('PATH', '')])
    env['CARGO_ENCODED_RUSTFLAGS'] = '\x1f'.join(['-C', f'dlltool={llvm / "llvm-dlltool.exe"}'])
    paths = [*HERE.joinpath('src').rglob('*.rs'), *HERE.joinpath('include').glob('*.h'),
             *HERE.joinpath('tests/fixtures/metallib').glob('*'),
             HERE / 'Cargo.toml', HERE / 'Cargo.lock', HERE / 'vendor/source-manifest.json',
             HERE / 'tests/check_metallib_c_abi.py', HERE / 'tests/check_metallib_owned_c_abi.py',
             HERE / 'tests/check_compute_metallib_c_abi.py',
             HERE / 'test-macos-staged.py', HERE / 'tools/run_native_compiler_tests.py', Path(__file__).resolve()]
    sources = {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(paths)}
    header = output / 'header-check.c'
    header.write_text('#include "hackynavi_metallib.h"\n', encoding='ascii')
    zig = ROOT / '.tools/zig-x86_64-windows-0.17.0/zig.exe'
    base = ['--locked', '--offline', '--manifest-path', str(HERE / 'Cargo.toml')]
    dll = ROOT / 'build/metal-compiler/debug/hackynavi_metal_compiler.dll'
    cli = ROOT / 'build/metal-compiler/debug/hackynavi-shader.exe'
    commands = [
        [str(zig), 'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-c',
         '-I' + str(HERE / 'include'), str(header), '-o', str(output / 'header-c.o')],
        [str(zig), 'c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-c', '-x', 'c++',
         '-I' + str(HERE / 'include'), str(header), '-o', str(output / 'header-cpp.o')],
        [str(cargo), 'test', *base, '--lib', '--', '--nocapture'],
        [str(cargo), 'build', *base, '--lib', '--bin', 'hackynavi-shader'],
        [sys.executable, str(HERE / 'tests/check_metallib_owned_c_abi.py'), str(dll), str(output / 'owned-abi.json')],
        [sys.executable, str(HERE / 'tests/check_compute_metallib_c_abi.py'), str(dll), str(output / 'compute-abi.json')],
        [sys.executable, str(HERE / 'tests/check_metallib_c_abi.py'), str(dll), str(cli), str(output / 'compiler-regression.json')],
    ]
    report = {'success': False, 'complete': False, 'hardware_executed': False, 'metal_device_registered': False,
              'sources': sources, 'commands': [], 'output': str(output)}
    def persist():
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    persist()
    for index, argv in enumerate(commands):
        result = subprocess.run(argv, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(result.stdout, end='', flush=True)
        (output / f'command-{index}.log').write_text(result.stdout, encoding='utf-8')
        report['commands'].append({'argv': argv, 'exit_code': result.returncode,
            'output_sha256': hashlib.sha256(result.stdout.encode()).hexdigest()})
        persist()
        if result.returncode:
            break
    report['snapshot_matches_worktree'] = all(hashlib.sha256((ROOT / name).read_bytes()).hexdigest() == value
                                             for name, value in sources.items())
    report['complete'] = True
    report['success'] = len(report['commands']) == len(commands) and all(c['exit_code'] == 0 for c in report['commands']) and report['snapshot_matches_worktree']
    if report['success']:
        report['artifacts'] = {p.name: {'bytes': p.stat().st_size, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                               for p in [dll, cli, output / 'owned-abi.json', output / 'compute-abi.json', output / 'compiler-regression.json']}
    persist()
    return 0 if report['success'] else 1

if __name__ == '__main__':
    raise SystemExit(main())
