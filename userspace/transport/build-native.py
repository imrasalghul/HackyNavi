#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile/link the real IOKit client on an existing Mac; sanitize protocol tests.

Uses a small unique staging directory, exports compile evidence, then removes
that exact directory. Does not install a library/kext or open a GPU connection.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import shlex
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--no-agent', action='store_true',
                        help='Use existing SSH key files without consulting the SSH agent')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/native-transport')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_.@-]+', args.host) or args.host.startswith('-'):
        parser.error('Expected an SSH host alias or user@host')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if (output / 'report.json').exists():
        parser.error('Output already contains evidence; select a new --output path')
    paths = [Path(__file__).resolve(), ROOT / 'include/hackynavi/native_abi.hpp',
             ROOT / 'include/hackynavi/gfx11_color_target.h', ROOT / 'tests/native_abi_tests.cpp']
    paths += [ROOT / 'userspace/metal/include' / name for name in
              ('hackynavi_shader.h', 'hackynavi_dispatch.h', 'hackynavi_pipeline.h')]
    paths += sorted((ROOT / 'userspace/transport').glob('*.cpp'))
    paths += sorted((ROOT / 'userspace/transport').glob('*.hpp'))
    captured = {p.relative_to(ROOT).as_posix(): p.read_bytes() for p in paths}
    report = {'files': {name: hashlib.sha256(data).hexdigest() for name, data in captured.items()},
              'kernel_loaded': False, 'runtime_gpu_tested': False, 'smoke_tool_executed': False}
    report['source_bytes'] = sum(len(data) for data in captured.values())
    if report['source_bytes'] > 2 * 1024 * 1024:
        raise ValueError('Refusing unexpectedly large transport source stage')
    manifest = ''.join(f'{digest}  {name}\n' for name, digest in report['files'].items()).encode('utf-8')
    source = output / 'source.tar.gz'
    with tarfile.open(source, 'w:gz') as tar:
        for name, data in [*captured.items(), ('source.sha256', manifest)]:
            entry = tarfile.TarInfo(name); entry.size = len(data); entry.mode = 0o644
            tar.addfile(entry, io.BytesIO(data))
    report['source_archive_sha256'] = hashlib.sha256(source.read_bytes()).hexdigest()
    connection_options = ['-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10']
    if args.no_agent:
        connection_options += ['-o', 'IdentityAgent=none']
    ssh = ['ssh', *connection_options, args.host]
    scp = ['scp', '-q', *connection_options]
    report['no_agent'] = args.no_agent
    report['remote_job_started'] = report['remote_job_finished'] = False
    report['remote_stage_removed'] = False
    (output / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    setup = ('set -eu; test "$(uname -s)" = Darwin; '
             'avail=$(df -Pk "$HOME" | tail -1 | awk \'{print $4}\'); '
             'test "$avail" -ge 1048576; '
             'mktemp -d "$HOME/Library/Caches/HackyNavi-transport.XXXXXX"')
    try:
        remote = subprocess.check_output(ssh + [setup], text=True, timeout=30).strip()
    except (subprocess.SubprocessError, OSError) as error:
        report['setup_error'] = str(error)
        report['remote_job_state'] = 'no compiler launched; stage creation outcome unknown'
        (output / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        return 1
    if not re.fullmatch(r'/Users/[^/\r\n]+/Library/Caches/HackyNavi-transport\.[A-Za-z0-9]+', remote):
        raise ValueError(f'Unexpected staging path: {remote!r}')
    report['remote_stage'] = remote
    q = shlex.quote(remote)
    common = 'clang++ -std=c++17 -arch x86_64 -mmacosx-version-min=15.0 -Wall -Wextra -Wpedantic -Werror -Iinclude '
    commands = [
        'set -eu', 'cd ' + q, 'tar -xzf source.tar.gz', 'shasum -a 256 -c source.sha256',
        'sw_vers', 'xcrun --sdk macosx --show-sdk-version',
        common + '-O2 -dynamiclib userspace/transport/native_protocol.cpp userspace/transport/iokit_client.cpp '
        '-framework IOKit -framework CoreFoundation -o libHackyNaviTransport.dylib',
        common + '-O2 userspace/transport/native_protocol.cpp userspace/transport/iokit_client.cpp '
        'userspace/transport/probe.cpp -framework IOKit -framework CoreFoundation -o hackynavi-transport-probe',
        common + '-O2 userspace/transport/native_protocol.cpp userspace/transport/iokit_client.cpp '
        'userspace/transport/compute-smoke.cpp -framework IOKit -framework CoreFoundation -o hackynavi-compute-smoke',
        common + '-O2 userspace/transport/native_protocol.cpp userspace/transport/iokit_client.cpp '
        'userspace/transport/compiler-smoke.cpp -framework IOKit -framework CoreFoundation -o hackynavi-compiler-smoke',
        common + '-O2 -Iuserspace/metal/include userspace/transport/native_protocol.cpp '
        'userspace/transport/iokit_client.cpp userspace/transport/product-smoke.cpp '
        '-framework IOKit -framework CoreFoundation -o hackynavi-product-smoke',
        common + '-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer '
        'userspace/transport/native_protocol.cpp tests/native_abi_tests.cpp -o native-abi-tests',
        'ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 ./native-abi-tests',
        'file libHackyNaviTransport.dylib hackynavi-transport-probe hackynavi-compute-smoke hackynavi-compiler-smoke hackynavi-product-smoke',
        'otool -L libHackyNaviTransport.dylib', 'du -sk .',
        'shasum -a 256 libHackyNaviTransport.dylib hackynavi-transport-probe hackynavi-compute-smoke hackynavi-compiler-smoke hackynavi-product-smoke > artifacts.sha256',
        'cat artifacts.sha256',
        'tar -czf artifacts.tar.gz artifacts.sha256 libHackyNaviTransport.dylib hackynavi-transport-probe hackynavi-compute-smoke hackynavi-compiler-smoke hackynavi-product-smoke'
    ]
    report['commands'] = commands
    remote_job_started = False
    remote_job_finished = False
    report['artifacts_exported'] = False
    (output / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    try:
        subprocess.run(scp + [str(source), args.host + ':' + remote + '/source.tar.gz'], check=True, timeout=60)
        remote_job_started = True
        report['remote_job_started'] = True
        (output / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        # Binary stdin preserves LF on Windows; an interrupted SSH process does
        # not prove its remote compiler has stopped.
        result = subprocess.run(ssh + ['sh -s'], input=('\n'.join(commands) + '\n').encode('utf-8'), capture_output=True, timeout=300)
        remote_job_finished = result.returncode >= 0 and result.returncode != 255
        report['exit_code'] = result.returncode
        log = (result.stdout + result.stderr).decode('utf-8', errors='replace')
        (output / 'build.log').write_text(log, encoding='utf-8')
        print(log, flush=True)
        if result.returncode == 0:
            archive = output / 'artifacts.tar.gz'
            subprocess.run(scp + [args.host + ':' + remote + '/artifacts.tar.gz', str(archive)], check=True, timeout=60)
            report['artifacts_sha256'] = hashlib.sha256(archive.read_bytes()).hexdigest()
            expected = {'libHackyNaviTransport.dylib', 'hackynavi-transport-probe', 'hackynavi-compute-smoke',
                        'hackynavi-compiler-smoke', 'hackynavi-product-smoke'}
            with tarfile.open(archive, 'r:gz') as tar:
                members = tar.getmembers()
                if {m.name for m in members} != expected | {'artifacts.sha256'} or len(members) != len(expected) + 1:
                    raise ValueError('Unexpected or duplicate artifact archive entry')
                if any(not m.isfile() or m.size < 0 for m in members) or sum(m.size for m in members) > 64 * 1024 * 1024:
                    raise ValueError('Unexpected artifact type/size')
                artifacts = {m.name: tar.extractfile(m).read() for m in members}
            manifest_hashes = {}
            for line in artifacts['artifacts.sha256'].decode('ascii').splitlines():
                match = re.fullmatch(r'([0-9a-f]{64})  ([A-Za-z0-9_.-]+)', line)
                if not match or match[2] in manifest_hashes:
                    raise ValueError('Invalid artifact hash manifest')
                manifest_hashes[match[2]] = match[1]
            if set(manifest_hashes) != expected or any(hashlib.sha256(artifacts[name]).hexdigest() != digest
                                                       for name, digest in manifest_hashes.items()):
                raise ValueError('Native artifact hash mismatch')
            report['artifact_files'] = {name: {'sha256': manifest_hashes[name], 'bytes': len(artifacts[name])}
                                        for name in sorted(expected)}
            for name, data in artifacts.items():
                (output / name).write_bytes(data)
            report['artifacts_exported'] = True
    except (subprocess.SubprocessError, OSError, ValueError, tarfile.TarError) as error:
        report['error'] = str(error)
        report.setdefault('exit_code', 1)
    finally:
        report['remote_job_started'] = remote_job_started
        report['remote_job_finished'] = remote_job_finished
        if not remote_job_started or (remote_job_finished and
                                      (report.get('exit_code') != 0 or report['artifacts_exported'])):
            # q refers only to the exact, parent/prefix-validated mktemp path.
            try:
                result = subprocess.run(ssh + ['rm -rf -- ' + q + '; test ! -e ' + q],
                                        capture_output=True, text=True, timeout=30)
                report['remote_stage_removed'] = result.returncode == 0
            except (subprocess.SubprocessError, OSError) as error:
                report['cleanup_error'] = str(error)
                report['remote_stage_removed'] = False
        else:
            report['remote_stage_removed'] = False
            report['remote_job_state'] = ('unknown; inspect live processes before rerun or cleanup'
                                          if not remote_job_finished else 'finished; preserve unexported artifacts')
        report['snapshot_matches_worktree'] = all((ROOT / name).is_file() and (ROOT / name).read_bytes() == data
            for name, data in captured.items())
        report['success'] = (report.get('exit_code') == 0 and report['artifacts_exported'] and
                             report['remote_stage_removed'] and report['snapshot_matches_worktree'])
        (output / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        print('remote_stage_removed:', report['remote_stage_removed'], flush=True)
    return 0 if report.get('success') else 1


if __name__ == '__main__':
    raise SystemExit(main())
