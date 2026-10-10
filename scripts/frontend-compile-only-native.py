#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile only the frozen frontend sources; never link or execute an artifact."""
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tarfile
import time

MAX_STAGE = 64 * 1024 * 1024


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    stage = Path(sys.argv[1]).resolve()
    if (sys.platform != 'darwin' or os.uname().machine != 'x86_64' or
            not re.fullmatch(r'/Users/[^/\r\n]+/Library/Caches/HackyNavi-FrontendObjects\.[A-Za-z0-9]+', str(stage)) or
            Path(__file__).resolve() != stage / 'scripts/frontend-compile-only-native.py'):
        raise RuntimeError('Not the exact isolated Intel Mac stage')
    os.chdir(stage)
    manifest = json.loads((stage / 'compile-manifest.json').read_text())
    sources = manifest['translation_units']
    if len(sources) != 22 or len(set(sources)) != 22:
        raise RuntimeError('Expected exactly 22 unique frontend translation units')
    if (stage / 'native-report.json').exists():
        raise RuntimeError('This stage has already been launched; never replay it')
    report = {'stage': str(stage), 'complete': False, 'success': False,
              'child_processes_reaped': True, 'commands': [], 'objects': {},
              'gpu_executed': False, 'metal_objects_created': False, 'context_open_called': False,
              'artifacts_executed': False, 'linked': False, 'tests_executed': False,
              'manifest_sha256': sha(stage / 'compile-manifest.json')}
    started = time.monotonic()
    env = os.environ.copy()
    for name in ('CPATH', 'CPLUS_INCLUDE_PATH', 'C_INCLUDE_PATH', 'OBJC_INCLUDE_PATH',
                 'SDKROOT', 'MACOSX_DEPLOYMENT_TARGET'):
        env.pop(name, None)
    (stage / 'tmp').mkdir()
    (stage / 'objects').mkdir()
    (stage / 'dependencies').mkdir()
    env['TMPDIR'] = str(stage / 'tmp')
    env['CLANG_MODULE_CACHE_PATH'] = str(stage / 'tmp/modules')

    def persist():
        temp = stage / 'native-report.new'
        temp.write_text(json.dumps(report, indent=2) + '\n')
        temp.replace(stage / 'native-report.json')

    def verify_sources():
        for name, expected in manifest['source_files'].items():
            path = stage / name
            if path.is_symlink() or not path.is_file() or path.stat().st_size != expected['bytes'] or sha(path) != expected['sha256']:
                raise RuntimeError('Frozen source changed: ' + name)

    def measure():
        used = sum(p.stat().st_size for p in stage.rglob('*') if p.is_file())
        report['max_observed_stage_bytes'] = max(used, report.get('max_observed_stage_bytes', 0))
        if used > MAX_STAGE:
            raise RuntimeError('Compile-only stage exceeded 64 MiB')

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
            until = time.monotonic() + 2
            while alive(process.pid) and time.monotonic() < until:
                time.sleep(.05)
            if not alive(process.pid):
                return True
        return False

    def run(argv):
        if time.monotonic() - started >= 540:
            raise RuntimeError('Total compile-only deadline reached')
        item = {'argv': argv, 'running': False}
        report['commands'].append(item)
        persist()
        process = subprocess.Popen(argv, cwd=stage, env=env, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, start_new_session=True)
        item.update(pid=process.pid, running=True)
        persist()
        try:
            stdout, stderr = process.communicate(timeout=min(90, 540 - (time.monotonic() - started)))
            if alive(process.pid) and not reap(process):
                report['child_processes_reaped'] = False
                raise RuntimeError('Compiler process group remains active')
            # Diagnostic storage is bounded; the original byte counts remain explicit.
            item.update(returncode=process.returncode, stdout=stdout[:262144], stderr=stderr[:262144],
                        stdout_chars=len(stdout), stderr_chars=len(stderr), running=False)
        except BaseException:
            if not reap(process):
                report['child_processes_reaped'] = False
            item['running'] = alive(process.pid)
            item['interrupted'] = True
            persist()
            raise
        measure()
        persist()
        if process.returncode:
            raise RuntimeError('Actual compiler command failed: ' + argv[0])
        return item['stdout'].strip()

    def interrupted(signum, frame):
        del frame
        raise RuntimeError('Interrupted by signal ' + str(signum))

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGHUP, interrupted)
    persist()
    try:
        verify_sources()
        report['os'] = run(['sw_vers'])
        report['compiler'] = run(['xcrun', '--sdk', 'macosx', 'clang++', '--version'])
        report['sdk_version'] = run(['xcrun', '--sdk', 'macosx', '--show-sdk-version'])
        if report['sdk_version'] != '26.2':
            raise RuntimeError('Expected actual SDK 26.2')
        sdk = Path(run(['xcrun', '--sdk', 'macosx', '--show-sdk-path'])).resolve()
        if not sdk.is_dir():
            raise RuntimeError('Missing actual SDK')
        report['sdk_path'] = str(sdk)
        common = ['xcrun', '--sdk', 'macosx', 'clang++', '-arch', 'x86_64',
                  '-isysroot', str(sdk), '-mmacosx-version-min=15.0', '-std=c++17',
                  '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-O2', '-ferror-limit=20', '-Iinclude']
        for index, name in enumerate(sources):
            path = Path(name)
            if name not in manifest['source_files'] or path.suffix not in ('.cpp', '.mm'):
                raise RuntimeError('Invalid translation unit')
            obj = 'objects/%02d-%s.o' % (index, path.stem)
            dep = 'dependencies/%02d-%s.d' % (index, path.stem)
            run(common + (['-fobjc-arc'] if path.suffix == '.mm' else []) +
                ['-MMD', '-MF', dep, '-c', name, '-o', obj])
            if (stage / obj).stat().st_size > 4 * 1024 * 1024:
                raise RuntimeError('Unexpected object size')
            report['objects'][obj] = {'source': name, 'bytes': (stage / obj).stat().st_size,
                                      'sha256': sha(stage / obj)}
            persist()
        verify_sources()
        report['sources_unchanged'] = True
        report['success'] = len(report['objects']) == 22
    except BaseException as error:
        report['error'] = str(error)
    finally:
        try:
            verify_sources()
            report['sources_unchanged'] = True
            if report['child_processes_reaped']:
                paths = sorted([*stage.glob('objects/*.o'), *stage.glob('dependencies/*.d')])
                report['artifacts'] = {p.relative_to(stage).as_posix(): {'bytes': p.stat().st_size, 'sha256': sha(p)} for p in paths}
                with tarfile.open(stage / 'artifacts.tar.gz', 'w:gz') as tar:
                    for path in paths:
                        tar.add(path, arcname=path.relative_to(stage).as_posix(), recursive=False)
                report['artifact_archive_sha256'] = sha(stage / 'artifacts.tar.gz')
                measure()
        except BaseException as error:
            report['success'] = False
            report['finalization_error'] = str(error)
        report['elapsed_seconds'] = time.monotonic() - started
        report['complete'] = True
        report['success'] = bool(report['success'] and report['child_processes_reaped'] and report.get('sources_unchanged'))
        persist()
    return 0 if report['success'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
