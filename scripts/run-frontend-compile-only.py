#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run a prepared object-only frontend compilation on the reserved Mac lane."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import shlex
import subprocess
import tarfile

STAGE_PATTERN = r'/Users/[^/\r\n]+/Library/Caches/HackyNavi-FrontendObjects\.[A-Za-z0-9]+'


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def load_preparation(prepared):
    freeze_bytes = (prepared / 'source-freeze.json').read_bytes()
    freeze = json.loads(freeze_bytes)
    archive = (prepared / 'inputs.tar.gz').read_bytes()
    bootstrap = (prepared / 'bootstrap.py').read_bytes()
    manifest_bytes = (prepared / 'compile-manifest.json').read_bytes()
    if (not freeze['prepared'] or freeze['remote_executed'] or len(archive) != freeze['archive_bytes'] or
            sha(archive) != freeze['archive_sha256'] or sha(bootstrap) != freeze['bootstrap_sha256'] or
            sha(manifest_bytes) != freeze['manifest_sha256'] or len(archive) > 4 * 1024 * 1024):
        raise RuntimeError('Prepared input digest/state mismatch')
    manifest = json.loads(manifest_bytes)
    if (manifest['source_files'] != freeze['source_files'] or
            manifest['translation_units'] != freeze['translation_units'] or
            len(manifest['translation_units']) != 22):
        raise RuntimeError('Prepared source plan differs')
    captured = {}
    with tarfile.open(fileobj=io.BytesIO(archive), mode='r:gz') as tar:
        members = tar.getmembers()
        expected = set(freeze['source_files']) | {'compile-manifest.json'}
        if len(members) != len(expected) or {m.name for m in members} != expected or sum(m.size for m in members) > 4 * 1024 * 1024:
            raise RuntimeError('Input members/bound mismatch')
        for member in members:
            if not member.isfile() or any(p in ('', '.', '..') or '\\' in p or ':' in p for p in member.name.split('/')):
                raise RuntimeError('Unsafe source member')
            value = tar.extractfile(member).read()
            if member.name == 'compile-manifest.json':
                if value != manifest_bytes:
                    raise RuntimeError('Manifest member differs')
            else:
                record = freeze['source_files'][member.name]
                if len(value) != record['bytes'] or sha(value) != record['sha256']:
                    raise RuntimeError('Input member digest/size mismatch')
            captured[member.name] = value
    return freeze_bytes, freeze, archive, bootstrap, manifest_bytes, captured


def cleanup_allowed(report, terminal):
    # Once launch may have occurred, preserve unknown state or unexported work.
    return not report['remote_job_started'] or (terminal and report['artifacts_exported'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepared', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--host', default='victor@mini')
    parser.add_argument('--no-agent', action='store_true')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_.@-]+', args.host) or args.host.startswith('-'):
        parser.error('Invalid SSH host')
    output = args.output.resolve()
    if output.exists():
        parser.error('Use a new evidence directory; never overwrite or replay a stage')
    freeze_bytes, frozen, archive, bootstrap, manifest_bytes, captured = load_preparation(args.prepared.resolve())
    output.mkdir(parents=True)
    (output / 'inputs.tar.gz').write_bytes(archive)
    (output / 'bootstrap.py').write_bytes(bootstrap)
    (output / 'compile-manifest.json').write_bytes(manifest_bytes)
    (output / 'source-freeze.json').write_bytes(freeze_bytes)
    report = {'success': False, 'remote_job_started': False, 'remote_job_finished': False,
              'remote_stage_removed': False, 'artifacts_exported': False, 'native_compile_passed': False,
              'preparation_sha256': sha(freeze_bytes), 'archive_sha256': sha(archive),
              'orchestrator_sha256': sha(Path(__file__).read_bytes()), 'translation_units': frozen['translation_units'],
              'source_bytes': frozen['source_bytes'], 'source_files': frozen['source_files'],
              'gpu_executed': False, 'metal_objects_created': False, 'tests_executed': False,
              'artifacts_executed': False, 'linked': False, 'context_open_called': False}

    def persist():
        temporary = output / 'report.new'
        temporary.write_text(json.dumps(report, indent=2) + '\n')
        temporary.replace(output / 'report.json')

    options = ['-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10'] + (['-o', 'IdentityAgent=none'] if args.no_agent else [])
    ssh = ['ssh', *options, args.host]
    scp = ['scp', '-q', *options]
    stage = None
    terminal = False
    persist()
    try:
        setup = '''import json,os,platform,shutil,tempfile
from pathlib import Path
if platform.system()!='Darwin' or platform.machine()!='x86_64':raise RuntimeError('Expected Intel Mac')
home=Path.home();available=shutil.disk_usage(home).free
if available<256*1024*1024:raise RuntimeError('Less than 256MiB free')
stage=tempfile.mkdtemp(prefix='HackyNavi-FrontendObjects.',dir=home/'Library/Caches')
print(json.dumps({'stage':stage,'available_bytes':available}))
'''
        response = subprocess.check_output(ssh + ['python3 -c ' + shlex.quote(setup)], text=True, timeout=30)
        setup_result = json.loads(response)
        candidate = setup_result['stage']
        if not isinstance(candidate, str) or not re.fullmatch(STAGE_PATTERN, candidate):
            raise RuntimeError('Unexpected remote stage identity')
        stage = candidate
        report.update(remote_stage=stage, available_bytes_before=setup_result['available_bytes'])
        persist()
        subprocess.run(scp + [str(output / 'inputs.tar.gz'), str(output / 'bootstrap.py'), args.host + ':' + stage + '/'],
                       check=True, timeout=60)
        verify = ('import hashlib\nfrom pathlib import Path\ns=Path(' + repr(stage) + ')\n' +
                  'assert hashlib.sha256((s/"inputs.tar.gz").read_bytes()).hexdigest()==' + repr(sha(archive)) + '\n' +
                  'assert hashlib.sha256((s/"bootstrap.py").read_bytes()).hexdigest()==' + repr(sha(bootstrap)) + '\n')
        subprocess.run(ssh + ['python3 -c ' + shlex.quote(verify)], check=True, timeout=30, capture_output=True, text=True)
        report['remote_inputs_verified'] = True
        # Persist BEFORE SSH: timeout/255 must never be treated as no launch.
        report['remote_job_started'] = True
        persist()
        try:
            job = subprocess.run(ssh + ['python3 ' + shlex.quote(stage + '/bootstrap.py') + ' ' + shlex.quote(stage)],
                                 capture_output=True, text=True, timeout=600)
            report['ssh_exit_code'] = job.returncode
            (output / 'native.log').write_text(job.stdout + job.stderr)
        except subprocess.TimeoutExpired as error:
            report['ssh_timeout'] = True
            (output / 'native.log').write_text(str(error))
        persist()
        # A fresh terminal journal, not SSH status, proves completion/reaping.
        native_bytes = subprocess.check_output(ssh + ['cat ' + shlex.quote(stage + '/native-report.json')], timeout=30)
        (output / 'native-report.json').write_bytes(native_bytes)
        native = json.loads(native_bytes)
        report['native'] = native
        terminal = bool(native.get('complete') and native.get('child_processes_reaped') and
                        native.get('stage') == stage and native.get('manifest_sha256') == frozen['manifest_sha256'] and
                        not any(c.get('running') for c in native.get('commands', [])))
        report['remote_job_finished'] = terminal
        persist()
        if not terminal:
            raise RuntimeError('Native process termination unconfirmed; preserve exact stage')
        subprocess.run(scp + [args.host + ':' + stage + '/artifacts.tar.gz', str(output / 'artifacts.tar.gz')], check=True, timeout=60)
        # Execute the exact captured local verifier bytes, not a possibly changed worktree file.
        helper = 'scripts/prepare-frontend-compile-only.py'
        namespace = {'__name__': 'frozen_export_verifier', '__file__': str(Path(__file__).parent / Path(helper).name)}
        exec(compile(captured[helper], helper, 'exec'), namespace)
        namespace['verify_export'](output, output)
        report['artifacts_exported'] = True
        report['native_compile_passed'] = bool(native.get('success') and len(native.get('objects', {})) == 22)
        persist()
        if not report['native_compile_passed']:
            raise RuntimeError('Actual frontend compilation failed; exported partial objects and diagnostics')
    except (OSError, ValueError, KeyError, AssertionError, RuntimeError, subprocess.SubprocessError, tarfile.TarError) as error:
        report['error'] = str(error)
    finally:
        if stage and cleanup_allowed(report, terminal):
            cleanup = '''import json,re,shutil
from pathlib import Path
s=Path(STAGE)
if str(s.resolve())!=STAGE or not re.fullmatch(PATTERN,STAGE):raise RuntimeError('Cleanup stage mismatch')
if LAUNCHED:
 r=json.loads((s/'native-report.json').read_text())
 if not r.get('complete') or not r.get('child_processes_reaped') or r.get('stage')!=STAGE or any(c.get('running') for c in r.get('commands',[])):raise RuntimeError('Cleanup termination unconfirmed')
used=sum(p.stat().st_size for p in s.rglob('*') if p.is_file())
shutil.rmtree(s)
if s.exists():raise RuntimeError('Exact stage still exists')
print(json.dumps({'removed':True,'stage_bytes_before_removal':used}))
'''
            cleanup = 'STAGE=' + repr(stage) + '\nPATTERN=' + repr(STAGE_PATTERN) + '\nLAUNCHED=' + repr(report['remote_job_started']) + '\n' + cleanup
            try:
                result = subprocess.run(ssh + ['python3 -c ' + shlex.quote(cleanup)], capture_output=True, text=True, timeout=30)
                if result.returncode:
                    report['cleanup_error'] = result.stderr
                else:
                    observed = json.loads(result.stdout)
                    report['remote_stage_removed'] = observed['removed'] is True
                    report['stage_bytes_before_removal'] = observed['stage_bytes_before_removal']
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                report['cleanup_error'] = str(error)
        report['success'] = bool(report['native_compile_passed'] and report['artifacts_exported'] and report['remote_stage_removed'])
        report['immutable_local_inputs_verified'] = bool(sha((output / 'inputs.tar.gz').read_bytes()) == frozen['archive_sha256'] and
                                                         sha((output / 'bootstrap.py').read_bytes()) == frozen['bootstrap_sha256'])
        report['success'] &= report['immutable_local_inputs_verified']
        persist()
        print(json.dumps({k: report.get(k) for k in ('success', 'native_compile_passed', 'artifacts_exported', 'remote_stage_removed', 'error')}), flush=True)
    return 0 if report['success'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
