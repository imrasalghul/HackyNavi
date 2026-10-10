#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Native ASan/UBSan real graphics compiler/runtime integration, with no device."""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import time


def main():
    stage = Path(sys.argv[1]).resolve()
    if (not re.fullmatch(r'/Users/[^/\r\n]+/Library/Caches/HackyNavi-GraphicsRuntime\.[A-Za-z0-9]+', str(stage)) or
            Path(__file__).resolve() != stage / 'userspace/graphics/run_runtime_native.py'):
        raise RuntimeError('Unexpected graphics runtime stage')
    os.chdir(stage)
    report = {'success': False, 'complete': False, 'child_processes_reaped': True,
              'stage': str(stage), 'gpu_executed': False, 'metal_objects_created': False, 'context_opened': False,
              'sanitizer_scope': 'actual authored C++ runtime, snapshot, graphics admission and harness; reused compiler libraries unsanitized',
              'commands': []}

    def persist():
        (stage / 'native-report.json').write_text(json.dumps(report, indent=2) + '\n')

    def alive(pid):
        try:
            os.killpg(pid, 0)
            return True
        except ProcessLookupError:
            return False

    def reap(process):
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
            while alive(process.pid) and time.monotonic() < deadline:
                time.sleep(.05)
            if not alive(process.pid):
                return True
        return False

    env = os.environ.copy()
    env.update(ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1',
               METAL2VULKAN_LLVM_DIS='/nonexistent/explicit-air-required',
               METAL2VULKAN_SPIRV_VAL='/nonexistent/explicit-validator-required',
               HACKYNAVI_APPLE_LLVM_LIBRARY='/nonexistent/fixed-system-reader-required')

    def run(argv):
        row = {'argv': [str(x) for x in argv], 'exit_code': None}
        report['commands'].append(row)
        persist()
        process = subprocess.Popen(row['argv'], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, start_new_session=True)
        try:
            stdout, stderr = process.communicate(timeout=120)
        except subprocess.TimeoutExpired:
            row['timeout'] = True
            report['child_processes_reaped'] &= reap(process)
            raise RuntimeError('Native runtime deadline expired')
        finally:
            if alive(process.pid):
                report['child_processes_reaped'] &= reap(process)
        row.update(exit_code=process.returncode, stdout=stdout, stderr=stderr)
        persist()
        if process.returncode or not report['child_processes_reaped']:
            raise RuntimeError('Native runtime command failed: ' + stderr)
        return stdout

    try:
        manifest = json.loads((stage / 'manifest.json').read_text())
        if len(manifest['files']) > 64 or manifest['bytes'] > 60 * 1024 * 1024:
            raise RuntimeError('Snapshot exceeds budget')
        for name, value in manifest['files'].items():
            path = stage / name
            if path.is_symlink() or not path.resolve().is_relative_to(stage) or not path.is_file():
                raise RuntimeError('Invalid snapshot file')
            raw = path.read_bytes()
            if len(raw) != value['bytes'] or hashlib.sha256(raw).hexdigest() != value['sha256']:
                raise RuntimeError('Snapshot file changed')
        (stage / 'results').mkdir()
        report['platform'] = run(['/usr/bin/sw_vers'])
        report['sdk'] = run(['/usr/bin/xcrun', '--show-sdk-version'])
        clang = run(['/usr/bin/xcrun', '--sdk', 'macosx', '--find', 'clang++']).strip()
        if not Path(clang).is_absolute() or not Path(clang).is_file():
            raise RuntimeError('Actual native compiler unavailable')
        report['clang_sha256'] = hashlib.sha256(Path(clang).read_bytes()).hexdigest()
        report['clang_version'] = run([clang, '--version'])
        flags = [clang, '-arch', 'x86_64', '-mmacosx-version-min=15.0', '-std=c++17', '-O1', '-g',
                 '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-fsanitize=address,undefined',
                 '-fno-sanitize-recover=all', '-I' + str(stage / 'include')]
        objects = []
        report['local_dependencies'] = {}
        for source in ('userspace/runtime/graphics_shader.cpp', 'userspace/runtime/graphics_build.cpp',
                       'userspace/runtime/graphics_snapshot.cpp', 'userspace/runtime/graphics_buffer_snapshot.cpp',
                       'src/gfx11_graphics_product.cpp',
                       'userspace/graphics/graphics_build_native.cpp'):
            stem = Path(source).stem
            target, dependency = 'results/' + stem + '.o', stage / ('results/' + stem + '.d')
            run([*flags, '-MMD', '-MT', 'unit.o', '-MF', dependency, '-c', source, '-o', target])
            text = dependency.read_text().replace('\\\n', '')
            if not text.startswith('unit.o:'):
                raise RuntimeError('Unexpected dependency record')
            paths = shlex.split(text[len('unit.o:'):].replace('$$', '$'))
            if not paths or len(paths) > 64:
                raise RuntimeError('Dependency budget')
            for name in paths:
                path = (stage / name).resolve()
                if not path.is_relative_to(stage):
                    raise RuntimeError('Unexpected non-system external dependency')
                relative = path.relative_to(stage).as_posix()
                if relative not in manifest['files'] or hashlib.sha256(path.read_bytes()).hexdigest() != manifest['files'][relative]['sha256']:
                    raise RuntimeError('Included local source was not snapshot-bound')
                report['local_dependencies'][relative] = manifest['files'][relative]['sha256']
            objects.append(target)
        executable = 'results/graphics-runtime-tests'
        run([*flags, *objects, '-L' + str(stage), '-lHackyNaviGraphicsCompiler', '-lHackyNaviColorLayout',
             '-lhackynavi_metal_compiler', '-Wl,-rpath,@loader_path/..', '-o', executable])
        report['test_output'] = run([str(stage / executable), str(stage)])
        if not re.search(r'\b96 checks, 0 failures, 8 actual compiler-backed graphics pipelines;', report['test_output']):
            raise RuntimeError('Expected real integration checks did not complete')
        report['artifacts'] = {name: {'bytes': (stage / name).stat().st_size,
                                     'sha256': hashlib.sha256((stage / name).read_bytes()).hexdigest()}
                               for name in [*objects, executable]}
        report['inputs_unchanged'] = all(hashlib.sha256((stage / name).read_bytes()).hexdigest() == value['sha256']
                                        for name, value in manifest['files'].items())
        report['peak_stage_bytes'] = sum(p.stat().st_size for p in stage.rglob('*') if p.is_file())
        if not report['inputs_unchanged'] or report['peak_stage_bytes'] > 96 * 1024 * 1024:
            raise RuntimeError('Input mutation or stage budget exceeded')
        report['success'] = True
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        report['complete'] = True
        persist()
    print(json.dumps({'success': True, 'output': report['test_output']}))


if __name__ == '__main__':
    main()
