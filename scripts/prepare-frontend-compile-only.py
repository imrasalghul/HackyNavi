#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prepare source-only native frontend compilation or verify its exported objects. No SSH."""
import argparse
import ast
import gzip
import hashlib
import io
import json
from pathlib import Path
import re
import tarfile

ROOT = Path(__file__).resolve().parents[1]
BUILDER = 'userspace/runtime/build-frontend.py'
RUNNER = 'scripts/frontend-compile-only-native.py'
MAX_INPUT = 4 * 1024 * 1024


def sha(data):
    return hashlib.sha256(data).hexdigest()


def safe(name):
    return bool(name) and not any(p in ('', '.', '..') or '\\' in p or ':' in p for p in name.split('/'))


def prepare(output):
    if output.exists():
        raise RuntimeError('Choose a new output directory; evidence is immutable')
    builder = (ROOT / BUILDER).read_bytes()
    tree = ast.parse(builder)
    values = [n.value for n in tree.body if isinstance(n, ast.Assign) and
              any(isinstance(t, ast.Name) and t.id == 'SOURCES' for t in n.targets)]
    if len(values) != 1:
        raise RuntimeError('Expected one source list')
    allowed = (ast.List, ast.Tuple, ast.ListComp, ast.comprehension, ast.Name,
               ast.Load, ast.Store, ast.Constant, ast.BinOp, ast.Add)
    if any(not isinstance(n, allowed) for n in ast.walk(values[0])):
        raise RuntimeError('Source list no longer uses the reviewed static expression')
    sources = eval(compile(ast.Expression(values[0]), BUILDER, 'eval'), {'__builtins__': {}})
    if len(sources) != 22 or len(set(sources)) != 22 or not all(isinstance(s, str) and safe(s) for s in sources):
        raise RuntimeError('Expected exactly 22 safe unique sources')
    data = {BUILDER: builder}
    pending = sources + [RUNNER, 'scripts/prepare-frontend-compile-only.py']
    while pending:
        name = pending.pop()
        if name in data:
            continue
        path = (ROOT / name).resolve()
        if not path.is_relative_to(ROOT) or not safe(name):
            raise RuntimeError('Source path escapes repository')
        value = path.read_bytes()
        if len(value) > 1024 * 1024:
            raise RuntimeError('Single source exceeds 1 MiB')
        data[name] = value
        if len(data) > 256 or sum(map(len, data.values())) > MAX_INPUT:
            raise RuntimeError('Source snapshot exceeds bounds')
        for delimiter, include in re.findall(r'^\s*#(?:include|import)\s+(["<])([^">]+)[">]', value.decode(), re.M):
            if delimiter == '<' and not include.startswith('hackynavi/'):
                continue
            found = next((p for p in (path.parent / include, ROOT / 'include' / include) if p.is_file()), None)
            if found is None:
                raise RuntimeError('Missing local include: ' + include)
            pending.append(found.resolve().relative_to(ROOT).as_posix())
    files = {p: {'bytes': len(v), 'sha256': sha(v)} for p, v in sorted(data.items())}
    manifest = {'translation_units': sources, 'source_files': files, 'compile_only': True,
                'expected_sdk': '26.2', 'source_bytes': sum(map(len, data.values()))}
    data['compile-manifest.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    stream = io.BytesIO()
    with gzip.GzipFile(fileobj=stream, mode='wb', mtime=0) as zipped:
        with tarfile.open(fileobj=zipped, mode='w') as tar:
            for name, value in sorted(data.items()):
                member = tarfile.TarInfo(name)
                member.size = len(value)
                member.mode = 0o644
                tar.addfile(member, io.BytesIO(value))
    archive = stream.getvalue()
    bootstrap = '''import hashlib,io,json,os,re,runpy,sys,tarfile
from pathlib import Path
stage=Path(sys.argv[1]).resolve()
if not re.fullmatch(r'/Users/[^/\\r\\n]+/Library/Caches/HackyNavi-FrontendObjects\\.[A-Za-z0-9]+',str(stage)):raise RuntimeError('Stage mismatch')
if (stage/'native-report.json').exists():raise RuntimeError('Already launched; inspect report, do not replay')
blob=(stage/'inputs.tar.gz').read_bytes()
if len(blob)!=ARCHIVE_BYTES or hashlib.sha256(blob).hexdigest()!=ARCHIVE_SHA:raise RuntimeError('Archive mismatch')
with tarfile.open(fileobj=io.BytesIO(blob),mode='r:gz') as tar:
 members=tar.getmembers()
 if len(members)!=len(FILES) or {m.name for m in members}!=set(FILES):raise RuntimeError('Member set mismatch')
 if sum(m.size for m in members)>4*1024*1024:raise RuntimeError('Input size exceeded')
 for m in members:
  expected=FILES[m.name]
  if not m.isfile() or m.size!=expected['bytes']:raise RuntimeError('Member type/size mismatch')
  value=tar.extractfile(m).read()
  if hashlib.sha256(value).hexdigest()!=expected['sha256']:raise RuntimeError('Member digest mismatch')
  path=stage/m.name
  if path.exists() or path.is_symlink() or not path.resolve().is_relative_to(stage):raise RuntimeError('Unsafe existing member')
  path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(value)
sys.argv=[str(stage/'scripts/frontend-compile-only-native.py'),str(stage)]
runpy.run_path(sys.argv[0],run_name='__main__')
'''
    archive_files = {p: {'bytes': len(v), 'sha256': sha(v)} for p, v in data.items()}
    bootstrap = 'ARCHIVE_SHA=' + repr(sha(archive)) + '\nARCHIVE_BYTES=' + repr(len(archive)) + '\nFILES=' + repr(archive_files) + '\n' + bootstrap
    compile(bootstrap, '<frozen-bootstrap>', 'exec')
    for name, value in data.items():
        if name != 'compile-manifest.json' and (ROOT / name).read_bytes() != value:
            raise RuntimeError('Source changed during preparation: ' + name)
    output.mkdir(parents=True)
    (output / 'inputs.tar.gz').write_bytes(archive)
    (output / 'bootstrap.py').write_text(bootstrap, newline='\n')
    (output / 'compile-manifest.json').write_bytes(data['compile-manifest.json'])
    report = {'prepared': True, 'remote_executed': False, 'translation_units': sources,
              'source_files': files, 'source_bytes': manifest['source_bytes'],
              'archive_bytes': len(archive), 'archive_sha256': sha(archive),
              'manifest_sha256': sha(data['compile-manifest.json']),
              'bootstrap_sha256': sha((output / 'bootstrap.py').read_bytes()),
              'scope': 'All 22 frozen frontend TUs, strict real-SDK object compile only; no binary dependencies, linking, tests or GPU.'}
    (output / 'source-freeze.json').write_text(json.dumps(report, indent=2) + '\n')
    recipe = '''Parent-only execution recipe (this preparation made no remote call):
1. Reserve the mini lane; record fresh df -Pk "$HOME". Require at least 262144 KiB free.
2. Make one exact stage: mktemp -d "$HOME/Library/Caches/HackyNavi-FrontendObjects.XXXXXX".
   Record the returned absolute path; never interpolate an unvalidated path into cleanup.
3. Copy only this inputs.tar.gz and bootstrap.py to the exact stage. Verify their SHA-256
   against source-freeze.json, then record launch intent locally BEFORE invoking:
       python3 <exact-stage>/bootstrap.py <exact-stage>
   The bootstrap verifies every input; the native runner compiles all 22 objects only.
4. A disconnected SSH / timeout / exit255 is UNKNOWN, not completion. Do not restart the
   stage or delete it. Inspect native-report.json, recorded pid/running state and process
   group liveness. Only complete:true AND child_processes_reaped:true permit cleanup.
5. Export native-report.json and artifacts.tar.gz into a NEW local evidence directory,
   then run this script --verify-export DIR --preparation THIS_PREPARED_DIRECTORY.
   This verifies archive hash, member paths/sizes/digests and actual Mach-O object headers.
   A compile failure may export partial objects for diagnostics but is never build success.
6. Once terminal/reaped and exports verified, remove only the recorded absolute stage,
   revalidated against /Users/<user>/Library/Caches/HackyNavi-FrontendObjects.<id>.
   Prefer a bounded Python shutil.rmtree on that exact resolved path; require absence after.
   Preserve local sources, logs, reports and exported objects; record remote stage removal.
No compile/link/test or Metal object/GPU result is asserted until its actual report exists.
'''
    (output / 'EXECUTION.txt').write_text(recipe, newline='\n')
    print(json.dumps({'source_freeze': str(output / 'source-freeze.json'),
                      'sha256': sha((output / 'source-freeze.json').read_bytes()),
                      'sources': len(sources), 'source_bytes': manifest['source_bytes'], 'archive_bytes': len(archive)}))


def verify_export(output, preparation):
    frozen = json.loads((preparation / 'source-freeze.json').read_text())
    report_bytes = (output / 'native-report.json').read_bytes()
    report = json.loads(report_bytes)
    if not report['complete'] or not report['child_processes_reaped'] or report['manifest_sha256'] != frozen['manifest_sha256']:
        raise RuntimeError('Native termination/input identity unconfirmed')
    archive = (output / 'artifacts.tar.gz').read_bytes()
    if len(archive) > 64 * 1024 * 1024 or sha(archive) != report['artifact_archive_sha256']:
        raise RuntimeError('Export archive mismatch')
    members_data = {}
    with tarfile.open(fileobj=io.BytesIO(archive), mode='r:gz') as tar:
        members = tar.getmembers()
        expected = report['artifacts']
        if len(members) != len(expected) or {m.name for m in members} != set(expected) or sum(m.size for m in members) > 64 * 1024 * 1024:
            raise RuntimeError('Export members or size mismatch')
        for member in members:
            name = member.name
            if not member.isfile() or not re.fullmatch(r'(objects/\d{2}-[A-Za-z0-9_]+\.o|dependencies/\d{2}-[A-Za-z0-9_]+\.d)', name):
                raise RuntimeError('Unexpected export member')
            data = tar.extractfile(member).read()
            if len(data) != expected[name]['bytes'] or sha(data) != expected[name]['sha256']:
                raise RuntimeError('Export digest mismatch')
            if name.endswith('.o'):
                import struct
                if len(data) < 32 or struct.unpack_from('<IIII', data) != (0xfeedfacf, 0x01000007, 3, 1):
                    raise RuntimeError('Expected actual x86_64 Mach-O MH_OBJECT')
            members_data[name] = data
    if report['success'] and (len(report['objects']) != 22 or not report.get('sources_unchanged')):
        raise RuntimeError('Incomplete successful build')
    destination = output / 'verified-artifacts'
    if destination.exists():
        raise RuntimeError('Never overwrite earlier exported objects')
    destination.mkdir()
    for name, data in members_data.items():
        path = destination / name
        path.parent.mkdir(exist_ok=True)
        path.write_bytes(data)
    result = {'artifacts_verified': True, 'native_compile_success': report['success'],
              'native_report_sha256': sha(report_bytes), 'archive_sha256': sha(archive),
              'source_freeze_sha256': sha((preparation / 'source-freeze.json').read_bytes()),
              'objects_verified': len(report['objects']), 'remote_stage_removed': False}
    (output / 'export-verification.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--output', type=Path)
    group.add_argument('--verify-export', type=Path)
    parser.add_argument('--preparation', type=Path)
    args = parser.parse_args()
    if args.output:
        if args.preparation:
            parser.error('--preparation is for export verification only')
        prepare(args.output.resolve())
    else:
        if not args.preparation:
            parser.error('--verify-export requires --preparation')
        verify_export(args.verify_export.resolve(), args.preparation.resolve())


if __name__ == '__main__':
    main()
