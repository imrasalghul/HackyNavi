#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the existing DML32 CPU tests with a Mac's installed xcrun clang.

Copies only this small source directory, retrieves the executable and evidence,
then removes the exact unique remote stage. No installation or GPU access.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/native-display-dml')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_.@-]+', args.host) or args.host.startswith('-'):
        parser.error('Expected an SSH host alias or user@host')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    paths = sorted(p for p in HERE.rglob('*') if p.is_file() and '__pycache__' not in p.parts)
    dependencies = ('include/hackynavi/discovery.hpp', 'include/hackynavi/display_bandwidth.hpp',
                    'src/discovery.cpp', 'src/display_bandwidth.cpp', 'include/hackynavi/display_model_input.hpp',
                    'include/hackynavi/display_smu.hpp', 'include/hackynavi/ip_layout.hpp', 'include/hackynavi/rom.hpp',
                    'include/hackynavi/edid.hpp', 'src/display_model_input.cpp', 'src/ip_layout.cpp', 'src/rom.cpp')
    paths += [ROOT / p for p in dependencies]
    if any(p.is_symlink() for p in paths):
        raise RuntimeError('Refusing to package source symlinks')
    source_bytes = sum(p.stat().st_size for p in paths)
    if source_bytes > 2 * 1024 * 1024:
        raise RuntimeError('Display source exceeded the small-stage size limit')
    report = {
        'scope': 'Native CPU DML32 calculations and integer register encoding; no GPU or modeset evidence',
        'success': False, 'host': args.host, 'source_bytes': source_bytes,
        'files': {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},
    }
    source = output / 'source.tar.gz'
    with tarfile.open(source, 'w:gz') as archive:
        for path in paths:
            archive.add(path, arcname=path.relative_to(ROOT).as_posix())
    source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    report['source_archive_sha256'] = source_hash
    ssh = ['ssh', '-T', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10',
           '-o', 'ServerAliveInterval=10', '-o', 'ServerAliveCountMax=2', args.host]
    scp = ['scp', '-q', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10']

    def remote_python(script, timeout=240):
        return subprocess.run(ssh + ['python3 -'], input=script, capture_output=True,
                              text=True, timeout=timeout)

    stage = None
    try:
        setup = remote_python('''import json, pathlib, platform, shutil, subprocess, tempfile
if platform.system() != 'Darwin':
    raise RuntimeError('Expected an existing Mac build host')
cache = pathlib.Path.home() / 'Library/Caches'
free = shutil.disk_usage(cache).free
if free < 1024 * 1024 * 1024:
    raise RuntimeError('Less than 1 GiB free; refusing the build')
clang = subprocess.check_output(['xcrun', '--find', 'clang'], text=True, timeout=20).strip()
if not pathlib.Path(clang).is_file():
    raise RuntimeError('xcrun did not return an installed clang')
stage = tempfile.mkdtemp(prefix='HackyNavi-display.', dir=cache)
print(json.dumps({'stage': stage, 'free_bytes_before': free, 'clang': clang,
                  'machine': platform.machine()}))
''')
        if setup.returncode:
            raise RuntimeError(setup.stderr or setup.stdout)
        metadata = json.loads(setup.stdout)
        stage = metadata['stage']
        if not re.fullmatch(r'/Users/[^/\r\n]+/Library/Caches/HackyNavi-display\.[A-Za-z0-9_-]+', stage):
            stage = None
            raise RuntimeError('Unexpected staging path; refusing remote operations')
        report.update(metadata)
        print('Native DML stage:', stage, flush=True)
        guard = f'''import pathlib
stage = pathlib.Path({stage!r})
cache = (pathlib.Path.home() / 'Library/Caches').resolve()
if stage.is_symlink() or stage.resolve().parent != cache or not stage.name.startswith('HackyNavi-display.'):
    raise RuntimeError('Stage escaped the expected cache directory')
'''
        subprocess.run(scp + [str(source), args.host + ':' + stage + '/source.tar.gz'],
                       check=True, timeout=240)
        script = guard + f'''import hashlib, json, os, shutil, subprocess, sys, tarfile
source = stage / 'source.tar.gz'
if hashlib.sha256(source.read_bytes()).hexdigest() != {source_hash!r}:
    raise RuntimeError('Source archive hash mismatch')
with tarfile.open(source, 'r:gz') as archive:
    expected = set({[p.relative_to(ROOT).as_posix() for p in paths]!r})
    names = [member.name for member in archive.getmembers()]
    if len(names) != len(set(names)) or set(names) != expected:
        raise RuntimeError('Source archive member mismatch')
    for member in archive.getmembers():
        path = pathlib.PurePosixPath(member.name)
        if not member.isfile() or path.is_absolute() or '..' in path.parts:
            raise RuntimeError('Invalid source archive member')
    archive.extractall(stage)
clang = subprocess.check_output(['xcrun', '--find', 'clang'], text=True, timeout=20).strip()
environment = {{'clang': clang, 'python': sys.version,
    'macos': subprocess.check_output(['sw_vers'], text=True, timeout=20),
    'sdk': subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-version'], text=True, timeout=20).strip(),
    'compiler': subprocess.check_output([clang, '--version'], text=True, timeout=20)}}
(stage / 'environment.json').write_text(json.dumps(environment, indent=2) + '\\n')
command = [sys.executable, str(stage / 'userspace/display/build-test.py'), '--compiler', clang, '--sanitize']
env = os.environ.copy()
env['ASAN_OPTIONS'] = 'detect_leaks=0'
env['UBSAN_OPTIONS'] = 'halt_on_error=1'
with (stage / 'native-build.log').open('w') as log:
    try:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, env=env, timeout=390)
        exit_code = result.returncode
    except subprocess.TimeoutExpired:
        exit_code = 124
        log.write('Native DML build/test exceeded the timeout\\n')
artifacts = [stage / 'environment.json', stage / 'native-build.log']
build = stage / 'build/display-dml'
artifacts += [build / name for name in ('calculation', 'encoding', 'profile', 'report.json', 'test.log') if (build / name).is_file()]
with tarfile.open(stage / 'artifacts.tar.gz', 'w:gz') as archive:
    for path in artifacts:
        archive.add(path, arcname=path.relative_to(stage).as_posix())
artifact_hash = hashlib.sha256((stage / 'artifacts.tar.gz').read_bytes()).hexdigest()
stage_bytes = sum(p.stat().st_size for p in stage.rglob('*') if p.is_file())
print(json.dumps({{'exit_code': exit_code, 'stage_bytes': stage_bytes,
    'artifacts_sha256': artifact_hash, 'command': command, 'environment': environment}}))
'''
        result = remote_python(script, timeout=630)
        (output / 'transport.log').write_text(result.stdout + result.stderr, encoding='utf-8')
        if result.returncode:
            raise RuntimeError(result.stderr or result.stdout)
        build_result = json.loads(result.stdout)
        report.update(build_result)
        artifacts = output / 'artifacts.tar.gz'
        subprocess.run(scp + [args.host + ':' + stage + '/artifacts.tar.gz', str(artifacts)],
                       check=True, timeout=240)
        if hashlib.sha256(artifacts.read_bytes()).hexdigest() != build_result['artifacts_sha256']:
            raise RuntimeError('Retrieved artifact hash mismatch')
        with tarfile.open(artifacts, 'r:gz') as archive:
            for member in archive.getmembers():
                path = PurePosixPath(member.name)
                if not member.isfile() or path.is_absolute() or '..' in path.parts:
                    raise RuntimeError('Invalid result archive member')
            archive.extractall(output / 'artifacts', filter='data')
        report['success'] = build_result['exit_code'] == 0
        print((output / 'artifacts/native-build.log').read_text(), flush=True)
    except Exception as error:
        report['error'] = str(error)
        print('Native DML build failed:', error, flush=True)
    finally:
        if stage:
            try:
                cleanup = remote_python(guard + '''import shutil
shutil.rmtree(stage)
if stage.exists():
    raise RuntimeError('Stage remains after cleanup')
print('removed')
''')
                report['remote_stage_removed'] = cleanup.returncode == 0 and cleanup.stdout.strip() == 'removed'
                if not report['remote_stage_removed']:
                    report['cleanup_error'] = cleanup.stderr or cleanup.stdout
            except Exception as error:
                report['remote_stage_removed'] = False
                report['cleanup_error'] = str(error)
        else:
            report['remote_stage_removed'] = None
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print('Remote stage removed:', report['remote_stage_removed'], flush=True)
    return 0 if report['success'] and report['remote_stage_removed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
