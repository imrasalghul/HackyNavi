#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prepare/link/test the real compiler dylib on an existing Intel Mac.

--prepare-only performs no remote operation. Native execution installs no tools,
loads no kext, opens no GPU, and preserves the stage on uncertain process status.
"""
import argparse
import base64
import gzip
import hashlib
import io
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tarfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host')
    parser.add_argument('--prepare-only', action='store_true')
    parser.add_argument('--helper-only-from', type=Path,
                        help='Rebuild only the system AIR helper against a verified prior compiler report')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/metal-compiler/macos-native')
    args = parser.parse_args()
    if not args.prepare_only and (not args.host or not re.fullmatch(r'[A-Za-z0-9_.@-]+', args.host)
                                  or args.host.startswith('-')):
        parser.error('Supply an SSH host alias or user@hostname, or --prepare-only')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report_path = output / 'report.json'
    if report_path.exists():
        parser.error('Output already contains evidence; choose a new --output directory')
    paths = {
        'libhackynavi_metal_compiler.a': ROOT / 'build/metal-compiler/x86_64-apple-darwin/debug/libhackynavi_metal_compiler.a',
        'spirv-val': ROOT / '.tools/userspace/build-spirv-tools-macos/tools/spirv-val',
        'apple_add_one.ll': ROOT / 'build/metal-compiler/apple-input/add_one.ll',
        'apple_add_one.air': HERE / 'tests/fixtures/apple/add_one.air',
        'apple_air_dis.c': HERE / 'tools/apple_air_dis.c',
        'run-native.py': HERE / 'tools/run_native_compiler_tests.py',
        'test-macos-staged.py': Path(__file__).resolve(),
    }
    prior_compiler = None
    if args.helper_only_from:
        prior_path = args.helper_only_from.resolve()
        prior_bytes = prior_path.read_bytes()
        prior_compiler = json.loads(prior_bytes)
        if (not prior_compiler.get('success') or not prior_compiler.get('artifacts_exported')
                or not prior_compiler.get('remote_stage_removed')
                or len(prior_compiler.get('exports', [])) != 32
                or 'hn_shader_compile_with_tools' not in prior_compiler['exports']):
            raise RuntimeError('Expected a verified explicit-tools compiler report')
        del paths['libhackynavi_metal_compiler.a']
        paths['libhackynavi_metal_compiler.dylib'] = prior_path.parent / 'libhackynavi_metal_compiler.dylib'
        paths['prior-compiler-report.json'] = prior_path
    for name in ['check_c_abi.py', 'check_apple_reader.py', 'check_dispatch_c_abi.py',
                 'check_pipeline_c_abi.py', 'check_metallib_c_abi.py', 'check_metallib_owned_c_abi.py',
                 'check_compute_metallib_c_abi.py', 'check_explicit_tools_c_abi.py']:
        paths['tests/' + name] = HERE / 'tests' / name
    for name in ['add_one.ll', 'affine_xor.ll', 'apple/add_one.air']:
        paths['tests/fixtures/' + name] = HERE / 'tests/fixtures' / name
    for path in sorted((HERE / 'tests/fixtures/metallib').iterdir()):
        paths['tests/fixtures/metallib/' + path.name] = path
    for name in ['hackynavi_shader.h', 'hackynavi_dispatch.h', 'hackynavi_pipeline.h', 'hackynavi_metallib.h']:
        paths['include/' + name] = HERE / 'include' / name
        paths['userspace/metal/include/' + name] = HERE / 'include' / name
    # Compile the real runtime overload and owned Pipeline; no Context object or
    # IOKit implementation is linked into this offline compiler harness.
    for name in ['compute_shader.hpp', 'compute_shader.cpp', 'compute_context.hpp',
                 'pipeline_snapshot.cpp', 'tests/compute_shader_tools_tests.cpp']:
        paths['userspace/runtime/' + name] = ROOT / 'userspace/runtime' / name
    paths['userspace/transport/iokit_client.hpp'] = ROOT / 'userspace/transport/iokit_client.hpp'
    paths['include/hackynavi/native_abi.hpp'] = ROOT / 'include/hackynavi/native_abi.hpp'
    exports = ['hn_shader_abi_version', 'hn_shader_compile', 'hn_shader_compile_with_tools', 'hn_shader_result_words',
               'hn_shader_result_reflection', 'hn_shader_result_spirv_entry_point',
               'hn_shader_result_error', 'hn_shader_result_free',
               'hn_shader_dispatch_abi_version', 'hn_shader_plan_dispatch', 'hn_shader_dispatch_plan_error',
               'hn_shader_dispatch_plan_region_count', 'hn_shader_dispatch_plan_region', 'hn_shader_dispatch_plan_free',
               'hn_pipeline_abi_version', 'hn_pipeline_build', 'hn_pipeline_error', 'hn_pipeline_info',
               'hn_pipeline_variant', 'hn_pipeline_region', 'hn_pipeline_resource', 'hn_pipeline_blob_size',
               'hn_pipeline_copy_blob', 'hn_pipeline_free',
               'hn_metallib_abi_version', 'hn_metallib_parse', 'hn_metallib_error',
               'hn_metallib_function_count', 'hn_metallib_function_info',
               'hn_metallib_copy_name', 'hn_metallib_copy_air', 'hn_metallib_free']
    aco_report_path = ROOT / 'build/gfx11-compiler/darwin-build-report.json'
    aco_report_bytes = aco_report_path.read_bytes()
    aco_report = json.loads(aco_report_bytes)
    for name, digest in aco_report['adapter_sources'].items():
        if hashlib.sha256((ROOT / 'userspace/compiler' / name).read_bytes()).hexdigest() != digest:
            raise RuntimeError('Darwin ACO adapter source is stale: ' + name)
    paths['hackynavi-aco'] = Path(aco_report['binary'])
    # Read once: the archive and manifest are built from exactly these bytes.
    data = {name: path.read_bytes() for name, path in paths.items()}
    if prior_compiler:
        data['prior-compiler-report.json'] = prior_bytes
        if hashlib.sha256(data['libhackynavi_metal_compiler.dylib']).hexdigest() != prior_compiler['native']['dylib']['sha256']:
            raise RuntimeError('Prior compiler artifact hash mismatch')
        data['helper-only.json'] = json.dumps({'prior_report_sha256': hashlib.sha256(prior_bytes).hexdigest()}).encode()
    if hashlib.sha256(data['hackynavi-aco']).hexdigest() != aco_report['binary_sha256']:
        raise RuntimeError('Darwin ACO binary hash mismatch')
    data['header-check.c'] = (b'#include "hackynavi_pipeline.h"\n#include "hackynavi_shader.h"\n#include "hackynavi_metallib.h"\n'
        b'_Static_assert(sizeof(HNPipelineVariant)==72,"variant ABI");\n'
        b'_Static_assert(sizeof(HNPipelineRegion)==120,"region ABI");\n'
        b'_Static_assert(sizeof(HNShaderTools)==24,"explicit tools ABI");\n'
        b'_Static_assert(HN_SHADER_AIR==0 && HN_SHADER_LLVM_TEXT==1 && HN_SHADER_METALLIB==2,"input ABI");\n')
    data['exports.txt'] = ('\n'.join('_' + name for name in exports) + '\n').encode('ascii')
    sources = {name: {'bytes': len(value), 'sha256': hashlib.sha256(value).hexdigest()}
               for name, value in data.items()}
    source_bytes = sum(len(value) for value in data.values())
    if source_bytes > 192 * 1024 * 1024:
        raise RuntimeError('Refusing a native source stage larger than 192 MiB')
    objects_report_path = ROOT / 'build/metal-compiler/darwin-objects-report.json'
    objects_report_bytes = objects_report_path.read_bytes()
    objects_report = json.loads(objects_report_bytes)
    if prior_compiler:
        objects_report = {'source_sha256': prior_compiler['compiler_source_sha256']}
    elif (not objects_report.get('success') or not objects_report.get('sha2_objects_present')
          or objects_report['artifact']['sha256'] != sources['libhackynavi_metal_compiler.a']['sha256']):
        raise RuntimeError('Run build-darwin-objects.py for matching current objects and evidence')
    for name, digest in objects_report['source_sha256'].items():
        if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest:
            raise RuntimeError('Darwin object source is stale: ' + name)
    archive_path = output / 'inputs.tar.gz'
    with tarfile.open(archive_path, 'w:gz') as archive:
        for name, value in data.items():
            info = tarfile.TarInfo(name)
            info.size = len(value)
            info.mode = 0o755 if name in ('spirv-val', 'hackynavi-aco') else 0o644
            archive.addfile(info, io.BytesIO(value))
    report = {'prepared': True, 'success': False, 'source_files': sources, 'source_bytes': source_bytes,
              'archive_bytes': archive_path.stat().st_size,
              'archive_sha256': hashlib.sha256(archive_path.read_bytes()).hexdigest(),
              'darwin_objects_report_sha256': hashlib.sha256(objects_report_bytes).hexdigest(),
              'compiler_source_sha256': objects_report['source_sha256'],
              'aco_build_report_sha256': hashlib.sha256(aco_report_bytes).hexdigest(), 'exports': exports,
              'hardware_executed': False, 'metal_device_registered': False,
              'remote_job_started': False, 'remote_job_finished': False, 'remote_stage_removed': False,
              'artifacts_exported': False}
    if prior_compiler:
        report['helper_only'] = True
        report['compiler_reused_from_report_sha256'] = hashlib.sha256(prior_bytes).hexdigest()
        report['darwin_objects_report_sha256'] = prior_compiler['darwin_objects_report_sha256']

    def persist():
        report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')

    persist()
    print(f'Prepared {source_bytes:,} bytes, {report["archive_bytes"]:,} compressed; no toolchain installation', flush=True)
    if args.prepare_only:
        report['remote_execution_requested'] = False
        persist()
        return 0
    report['remote_execution_requested'] = True
    persist()
    # The Windows SSH agent can stall identity enumeration. Existing default
    # key files authenticate this validation without agent IPC or new secrets.
    ssh = ['ssh', '-o', 'BatchMode=yes', '-o', 'IdentityAgent=none', '-o', 'ConnectTimeout=15', args.host]
    setup = ('set -eu; test "$(uname -s)" = Darwin; '
             'test "$(uname -m)" = x86_64; '
             'avail=$(df -Pk "$HOME" | tail -1 | awk \'{print $4}\'); '
             'test "$avail" -ge 1048576; '
             'mktemp -d "$HOME/Library/Caches/HackyNavi-MetalNative.XXXXXX"')
    stage = subprocess.check_output(ssh + [setup], text=True, timeout=30).strip()
    if not re.fullmatch(r'/Users/[^/\r\n]+/Library/Caches/HackyNavi-MetalNative\.[A-Za-z0-9]+', stage):
        raise RuntimeError('Unexpected remote stage path: ' + repr(stage))
    report['remote_stage'] = stage
    persist()
    bootstrap = '''import hashlib,json,os,pathlib,sys,tarfile
stage=pathlib.Path(sys.argv[1])
expected=json.loads(MANIFEST)
archive=stage/'inputs.tar.gz'
if hashlib.sha256(archive.read_bytes()).hexdigest()!=ARCHIVE_SHA:
    raise RuntimeError('archive hash mismatch')
with tarfile.open(archive,'r:gz') as tar:
    seen=set()
    for member in tar:
        if member.name not in expected or member.name in seen or not member.isfile():
            raise RuntimeError('unexpected archive member')
        item=expected[member.name]
        if member.size!=item['bytes']:
            raise RuntimeError('archive member size mismatch')
        value=tar.extractfile(member).read()
        if len(value)!=item['bytes'] or hashlib.sha256(value).hexdigest()!=item['sha256']:
            raise RuntimeError('archive member hash mismatch')
        path=stage/member.name
        path.parent.mkdir(parents=True,exist_ok=True)
        path.write_bytes(value)
        path.chmod(0o755 if member.name in ('spirv-val','hackynavi-aco') else 0o644)
        seen.add(member.name)
    if seen!=set(expected):
        raise RuntimeError('archive is missing members')
os.execv('/usr/bin/python3',['/usr/bin/python3',str(stage/'run-native.py'),str(stage)])
'''.replace('MANIFEST', repr(json.dumps(sources))).replace('ARCHIVE_SHA', repr(report['archive_sha256']))
    compile(bootstrap, '<native-bootstrap>', 'exec')
    qstage = shlex.quote(stage)
    try:
        subprocess.run(['scp', '-q', '-o', 'BatchMode=yes', '-o', 'IdentityAgent=none', str(archive_path),
                        args.host + ':' + stage + '/inputs.tar.gz'], check=True, timeout=180)
        report['remote_job_started'] = True
        persist()
        result = subprocess.run(ssh + ['/usr/bin/python3 -c ' + shlex.quote(bootstrap) + ' ' + qstage],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=900)
        (output / 'transport-stderr.txt').write_bytes(result.stderr)
        if result.returncode != 0:
            raise RuntimeError('Native SSH job returned ' + str(result.returncode) + '; process state unconfirmed')
        native = json.loads(result.stdout)
        report['remote_job_finished'] = (native.get('complete') is True
                                         and native.get('child_processes_reaped') is True
                                         and native.get('staging') == stage)
        if not report['remote_job_finished']:
            raise RuntimeError('Native child termination unconfirmed; preserving stage')
        artifacts = [('dylib', 'libhackynavi_metal_compiler.dylib'), ('apple_air_dis', 'apple-air-dis')]
        for key, filename in artifacts:
            encoded = native.pop(key + '_gzip_base64', None)
            if encoded:
                value = gzip.decompress(base64.b64decode(encoded, validate=True))
                if len(value) != native[key]['bytes'] or hashlib.sha256(value).hexdigest() != native[key]['sha256']:
                    raise RuntimeError('Returned artifact hash/size mismatch: ' + key)
                (output / filename).write_bytes(value)
        report['native'] = native
        report['artifacts_exported'] = all((output / filename).is_file() for _, filename in artifacts)
        report['native_tests_passed'] = native.get('success') is True
        if 'metallib_integration' in native:
            (output / 'metallib-integration.json').write_text(
                json.dumps(native['metallib_integration'], indent=2) + '\n', encoding='utf-8')
        persist()
    finally:
        if not report['remote_job_started'] or report['remote_job_finished']:
            cleanup = subprocess.run(ssh + ['rm -rf -- ' + qstage + '; test ! -e ' + qstage],
                                     capture_output=True, text=True, timeout=30)
            report['remote_stage_removed'] = cleanup.returncode == 0
        else:
            report['remote_job_state'] = 'unknown; inspect live processes before any cleanup or rerun'
        report['snapshot_matches_worktree'] = all(path.read_bytes() == data[name] for name, path in paths.items())
        report['success'] = (report.get('native_tests_passed', False) and report['artifacts_exported']
                             and report['remote_stage_removed'] and report['snapshot_matches_worktree'])
        persist()
        print(json.dumps({key: report.get(key) for key in ['success', 'remote_stage', 'remote_stage_removed',
              'remote_job_finished', 'artifacts_exported', 'snapshot_matches_worktree']}, indent=2), flush=True)
    return 0 if report['success'] else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.TimeoutExpired, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
