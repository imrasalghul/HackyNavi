#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Native compiler validation in a verified private stage; no GPU or kext use.

Cleanup belongs to the caller after receiving the completed job/artifact report.
"""
import base64
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time


def main():
    stage = Path(sys.argv[1]).resolve()
    if not re.fullmatch(r'/Users/[^/\r\n]+/Library/Caches/HackyNavi-MetalNative\.[A-Za-z0-9]+', str(stage)):
        raise RuntimeError('unexpected private stage path')
    if Path(__file__).resolve().parent != stage:
        raise RuntimeError('runner is outside the supplied stage')
    report = {'claim': 'Rust objects cross-compiled on Windows; dylib linked and C ABI executed natively on Intel macOS',
              'staging': str(stage), 'commands': [], 'child_processes_reaped': True,
              'complete': False, 'success': False, 'hardware_executed': False,
              'metal_device_registered': False, 'cli_tested': False}
    environment = os.environ.copy()
    (stage / 'tmp').mkdir()
    environment['TMPDIR'] = str(stage / 'tmp')
    environment['METAL2VULKAN_SPIRV_VAL'] = str(stage / 'spirv-val')
    environment['METAL2VULKAN_LLVM_DIS'] = str(stage / 'apple-air-dis')

    def persist():
        (stage / 'native-report.json').write_text(json.dumps(report, indent=2) + '\n')

    def group_alive(pid):
        try:
            os.killpg(pid, 0)
            return True
        except ProcessLookupError:
            return False

    def stop_group(process):
        for sig in (signal.SIGTERM, signal.SIGKILL):
            try:
                os.killpg(process.pid, sig)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass
            deadline = time.monotonic() + 2
            while group_alive(process.pid) and time.monotonic() < deadline:
                time.sleep(0.05)
            if not group_alive(process.pid):
                return True
        return False

    def run(argv, command_environment=None):
        item = {'argv': argv, 'exit_code': None}
        report['commands'].append(item)
        persist()
        process = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, env=environment if command_environment is None else command_environment,
                                   start_new_session=True)
        try:
            stdout, stderr = process.communicate(timeout=180)
            if group_alive(process.pid) and not stop_group(process):
                report['child_processes_reaped'] = False
                raise RuntimeError('descendant group did not terminate; preserve stage')
            item.update({'exit_code': process.returncode, 'stdout': stdout, 'stderr': stderr})
        except BaseException:
            if not stop_group(process):
                report['child_processes_reaped'] = False
            item['error'] = 'command interrupted/timed out; child group termination attempted'
            persist()
            raise
        used = sum(p.stat().st_size for p in stage.rglob('*') if p.is_file())
        report['max_observed_stage_bytes'] = max(used, report.get('max_observed_stage_bytes', 0))
        persist()
        if used > 256 * 1024 * 1024:
            raise RuntimeError('stage exceeded 256 MiB observed size')
        if item['exit_code'] != 0:
            raise RuntimeError('native command failed: ' + argv[0])
        return item

    try:
        if (stage / 'helper-only.json').is_file():
            report['claim'] = 'Existing verified compiler reused unchanged; fixed-system AIR helper rebuilt and exercised natively'
            report['helper_only'] = True
            run(['sw_vers'])
            run(['xcrun', 'clang', '--version'])
            library = stage / 'libhackynavi_metal_compiler.dylib'
            prior = json.loads((stage / 'prior-compiler-report.json').read_text())
            if hashlib.sha256(library.read_bytes()).hexdigest() != prior['native']['dylib']['sha256']:
                raise RuntimeError('Reused compiler hash mismatch')
            run(['xcrun', '--sdk', 'macosx', 'clang', '-arch', 'x86_64', '-mmacosx-version-min=15.0',
                 '-Wall', '-Wextra', '-Werror', '-O2', str(stage / 'apple_air_dis.c'), '-o', str(stage / 'apple-air-dis')])
            run([str(stage / 'apple-air-dis'), '--version'])
            run(['/usr/bin/python3', str(stage / 'tests/check_apple_reader.py'),
                 str(stage / 'apple-air-dis'), str(stage / 'apple_add_one.air')])
            run(['/usr/bin/python3', str(stage / 'tests/check_explicit_tools_c_abi.py'), str(library),
                 str(stage / 'apple-air-dis'), str(stage / 'spirv-val'), str(stage / 'hackynavi-aco'),
                 str(stage / 'explicit-tools-integration.json')])
            explicit = json.loads((stage / 'explicit-tools-integration.json').read_text())
            if not explicit.get('success') or explicit.get('checks') != 94 or explicit.get('gpu_executed') is not False:
                raise RuntimeError('Fixed system helper actual compiler integration unconfirmed')
            report['explicit_tools_integration'] = explicit
            for name, key in [('libhackynavi_metal_compiler.dylib', 'dylib'), ('apple-air-dis', 'apple_air_dis')]:
                value = (stage / name).read_bytes()
                if len(value) > 64 * 1024 * 1024:
                    raise RuntimeError('unexpectedly large native artifact')
                report[key] = {'bytes': len(value), 'sha256': hashlib.sha256(value).hexdigest()}
                report[key + '_gzip_base64'] = base64.b64encode(gzip.compress(value)).decode('ascii')
            report['success'] = True
        else:
            run(['sw_vers'])
            run(['xcrun', 'clang', '--version'])
            run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsyntax-only',
                 '-I' + str(stage / 'include'), str(stage / 'header-check.c')])
            run(['xcrun', 'clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsyntax-only',
                 '-x', 'c++', '-I' + str(stage / 'include'), str(stage / 'header-check.c')])
            library = stage / 'libhackynavi_metal_compiler.dylib'
            run(['xcrun', '--sdk', 'macosx', 'clang', '-arch', 'x86_64', '-mmacosx-version-min=15.0', '-dynamiclib',
                 '-Wl,-force_load,' + str(stage / 'libhackynavi_metal_compiler.a'), '-Wl,-dead_strip',
                 '-Wl,-exported_symbols_list,' + str(stage / 'exports.txt'),
                 '-Wl,-install_name,@rpath/libhackynavi_metal_compiler.dylib', '-liconv', '-o', str(library)])
            run(['xcrun', '--sdk', 'macosx', 'clang', '-arch', 'x86_64', '-mmacosx-version-min=15.0',
                 '-Wall', '-Wextra', '-Werror', '-O2', str(stage / 'apple_air_dis.c'), '-o', str(stage / 'apple-air-dis')])
            run([str(stage / 'apple-air-dis'), '--version'])
            run(['/usr/bin/python3', str(stage / 'tests/check_apple_reader.py'),
                 str(stage / 'apple-air-dis'), str(stage / 'apple_add_one.air')])
            run(['xcrun', 'lipo', str(library), '-verify_arch', 'x86_64'])
            run(['xcrun', 'otool', '-L', str(library)])
            run([str(stage / 'spirv-val'), '--version'])
            for fixture in (None, 'apple_add_one.ll', 'apple_add_one.air'):
                command = ['/usr/bin/python3', str(stage / 'tests/check_c_abi.py'), str(library)]
                if fixture:
                    command.append(str(stage / fixture))
                run(command)
            run(['/usr/bin/python3', str(stage / 'tests/check_dispatch_c_abi.py'), str(library)])
            run(['/usr/bin/python3', str(stage / 'tests/check_pipeline_c_abi.py'), str(library), str(stage / 'hackynavi-aco')])
            run(['/usr/bin/python3', str(stage / 'tests/check_metallib_c_abi.py'), str(library), '-',
                 str(stage / 'metallib-integration.json')])
            integration = json.loads((stage / 'metallib-integration.json').read_text())
            if not integration.get('success') or integration.get('cli_tested') is not False:
                raise RuntimeError('metallib native C ABI completion unconfirmed')
            report['metallib_integration'] = integration
            run(['/usr/bin/python3', str(stage / 'tests/check_metallib_owned_c_abi.py'), str(library),
                 str(stage / 'metallib-owned-integration.json')])
            owned = json.loads((stage / 'metallib-owned-integration.json').read_text())
            if not owned.get('success') or owned.get('owned_air_compiled_functions') != 7 or owned.get('hardware_executed') is not False:
                raise RuntimeError('owned metallib native ABI completion unconfirmed')
            report['metallib_owned_integration'] = owned
            run(['/usr/bin/python3', str(stage / 'tests/check_compute_metallib_c_abi.py'), str(library),
                 str(stage / 'compute-metallib-integration.json')])
            compute = json.loads((stage / 'compute-metallib-integration.json').read_text())
            if not compute.get('success') or compute.get('checks') != 48 or compute.get('gpu_execution') is not False:
                raise RuntimeError('actual compute metallib ABI completion unconfirmed')
            report['compute_metallib_integration'] = compute
            run(['/usr/bin/python3', str(stage / 'tests/check_explicit_tools_c_abi.py'), str(library),
                 str(stage / 'apple-air-dis'), str(stage / 'spirv-val'), str(stage / 'hackynavi-aco'),
                 str(stage / 'explicit-tools-integration.json')])
            explicit = json.loads((stage / 'explicit-tools-integration.json').read_text())
            if not explicit.get('success') or explicit.get('gpu_executed') is not False:
                raise RuntimeError('explicit compiler tools ABI completion unconfirmed')
            report['explicit_tools_integration'] = explicit
            runtime = stage / 'userspace/runtime'
            runtime_test = stage / 'compute-shader-tools-tests'
            run(['xcrun', '--sdk', 'macosx', 'clang++', '-arch', 'x86_64',
                 '-mmacosx-version-min=15.0', '-std=c++17', '-O1', '-g',
                 '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-fsanitize=address,undefined',
                 '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
                 '-I' + str(stage / 'include'), str(runtime / 'compute_shader.cpp'),
                 str(runtime / 'pipeline_snapshot.cpp'), str(runtime / 'tests/compute_shader_tools_tests.cpp'),
                 str(library), '-Wl,-rpath,@loader_path', '-o', str(runtime_test)])
            runtime_environment = environment.copy()
            runtime_environment.update(PATH='missing-environment-search-path',
                                       METAL2VULKAN_LLVM_DIS='missing-environment-air-dis',
                                       METAL2VULKAN_SPIRV_VAL='missing-environment-spirv-val',
                                       # Apple's native ASan does not support LSan.
                                       ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',
                                       UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
            runtime_result = run([str(runtime_test), str(stage / 'apple_add_one.air'),
                                  str(stage / 'apple-air-dis'), str(stage / 'spirv-val'),
                                  str(stage / 'hackynavi-aco')], runtime_environment)
            runtime_report = json.loads(runtime_result['stdout'])
            if (runtime_report.get('success') is not True or runtime_report.get('checks') != 27
                    or runtime_report.get('gpu_executed') is not False
                    or runtime_report.get('context_opened') is not False):
                raise RuntimeError('actual C++ explicit-tools ownership checks unconfirmed')
            runtime_report['executable_sha256'] = hashlib.sha256(runtime_test.read_bytes()).hexdigest()
            runtime_report['environment_tool_overrides_invalid'] = True
            report['runtime_explicit_tools_integration'] = runtime_report
            for name, key in [('libhackynavi_metal_compiler.dylib', 'dylib'), ('apple-air-dis', 'apple_air_dis')]:
                value = (stage / name).read_bytes()
                if len(value) > 64 * 1024 * 1024:
                    raise RuntimeError('unexpectedly large native artifact')
                report[key] = {'bytes': len(value), 'sha256': hashlib.sha256(value).hexdigest()}
                report[key + '_gzip_base64'] = base64.b64encode(gzip.compress(value)).decode('ascii')
            report['success'] = True
    except BaseException as error:
        report['error'] = type(error).__name__ + ': ' + str(error)
    report['complete'] = True
    persist()
    print(json.dumps(report), flush=True)


if __name__ == '__main__':
    main()
