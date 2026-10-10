#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run actual-fixture metallib extraction and existing Rust library tests locally."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    local = ROOT / '.tools/userspace'
    cargo = local / 'cargo/bin/cargo.exe'
    llvm = local / 'src/clang+llvm-20.1.8-x86_64-pc-windows-msvc/bin'
    gcc = local / 'rustup/toolchains/1.90.0-x86_64-pc-windows-gnu/lib/rustlib/x86_64-pc-windows-gnu/bin/self-contained'
    env = os.environ.copy()
    env['RUSTUP_HOME'] = str(local / 'rustup')
    env['CARGO_HOME'] = str(local / 'cargo')
    env['PATH'] = os.pathsep.join([str(cargo.parent), str(gcc), env.get('PATH', '')])
    env['CARGO_TARGET_DIR'] = str(ROOT / 'build/metal-compiler')
    env['METAL2VULKAN_LLVM_DIS'] = str(llvm / 'llvm-dis.exe')
    env['METAL2VULKAN_SPIRV_VAL'] = str(local / 'build-spirv-tools/tools/spirv-val.exe')
    env['CARGO_ENCODED_RUSTFLAGS'] = '\x1f'.join(['-C', f'dlltool={llvm / "llvm-dlltool.exe"}'])
    paths = [*HERE.joinpath('src').rglob('*.rs'), *HERE.joinpath('include').glob('*.h'),
             HERE / 'Cargo.toml', HERE / 'Cargo.lock',
             HERE / 'METALLIB.md', Path(__file__).resolve(), HERE / 'vendor/source-manifest.json',
             *HERE.joinpath('tests/fixtures/metallib').glob('*'),
             HERE / 'tests/fixtures/affine_xor.ll', HERE / 'tests/fixtures/apple/add_one.air',
             HERE / 'tests/check_metallib_c_abi.py']
    hashes = {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in sorted(paths)}
    output = ROOT / 'build/metal-metallib'
    output.mkdir(parents=True, exist_ok=True)
    base = ['--locked', '--offline', '--manifest-path', str(HERE / 'Cargo.toml')]
    commands = [[str(cargo), 'test', *base, '--lib', '--', '--nocapture'],
                [str(cargo), 'build', *base, '--lib', '--bin', 'hackynavi-shader'],
                [sys.executable, str(HERE / 'tests/check_metallib_c_abi.py'),
                 str(ROOT / 'build/metal-compiler/debug/hackynavi_metal_compiler.dll'),
                 str(ROOT / 'build/metal-compiler/debug/hackynavi-shader.exe'),
                 str(output / 'compiler-integration.json')]]
    tool_paths = [cargo, llvm / 'llvm-dis.exe', local / 'build-spirv-tools/tools/spirv-val.exe']
    report = {'scope': 'real SDL MTLB 2.5 extraction, C ABI/CLI compilation and Rust regression tests',
              'hardware_executed': False, 'metal_device_registered': False,
              'modern_macos_toolchain_metallib_verified': False, 'sources': hashes,
              'tools': {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                        for p in tool_paths}, 'commands': [], 'complete': False}
    report_path = output / 'report.json'
    report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    logs = []
    for command in commands:
        result = subprocess.run(command, env=env, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        print(result.stdout, end='', flush=True)
        logs.append(result.stdout)
        report['commands'].append({'argv': command, 'exit_code': result.returncode})
        if result.returncode:
            break
    (output / 'test.log').write_text(''.join(logs), encoding='utf-8')
    report['snapshot_matches_worktree'] = all(
        hashlib.sha256((ROOT / p).read_bytes()).hexdigest() == digest for p, digest in hashes.items())
    report['complete'] = True
    report['success'] = (len(report['commands']) == len(commands)
                         and all(command['exit_code'] == 0 for command in report['commands'])
                         and report['snapshot_matches_worktree'])
    if report['success']:
        report['integration_report_sha256'] = hashlib.sha256(
            (output / 'compiler-integration.json').read_bytes()).hexdigest()
    report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return 0 if report['success'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
