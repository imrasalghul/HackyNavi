#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the actual native compute runtime and run only offline pipeline tests.

Stages an existing verified compiler dylib and tools once; no Rust build,
installation, Context::open, GPU test or kext load. Unknown jobs retain their
private stage. --prepare-only performs no SSH operation.
"""
import argparse,hashlib,io,json,re,shlex,subprocess,sys,tarfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]

def digest(data):return hashlib.sha256(data).hexdigest()
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host');parser.add_argument('--no-agent',action='store_true')
    parser.add_argument('--prepare-only',action='store_true')
    parser.add_argument('--prepared-from',type=Path,help='Run an exact previously prepared input archive without recapturing evolving sources')
    parser.add_argument('--relink-from',type=Path,help='Replace the ObjC buffer object in a verified prior runtime archive; no offline test rerun')
    parser.add_argument('--extend-shader-from',type=Path,help='Reuse verified runtime/Buffer objects, add Shader and Metal library objects, and run only new Shader tests')
    parser.add_argument('--extend-pipeline-from',type=Path,help='Reuse seven verified runtime/Shader/library objects, add Metal compute pipeline and shared ISA admission, and run focused actual compiler admission tests')
    parser.add_argument('--extend-command-from',type=Path,help='Reuse nine verified runtime objects, add actual Metal command objects and host FIFO lifecycle, and test only new host lifecycle logic')
    parser.add_argument('--compiler-report',type=Path,help='Successful verified native compiler report with matching exported dylib and headers')
    parser.add_argument('--output',type=Path,default=ROOT/'build/native-compute-runtime')
    args=parser.parse_args()
    if args.prepared_from and (args.prepare_only or args.relink_from or args.extend_shader_from or args.extend_pipeline_from or args.extend_command_from or args.compiler_report):
        parser.error('--prepared-from cannot be combined with source preparation options')
    if (args.extend_shader_from or args.extend_pipeline_from or args.extend_command_from) and (args.relink_from or not args.compiler_report):
        parser.error('Runtime extension requires --compiler-report and cannot be combined with --relink-from')
    if sum(bool(value) for value in [args.extend_shader_from,args.extend_pipeline_from,args.extend_command_from])>1:
        parser.error('Select exactly one runtime extension mode')
    if not args.prepare_only and (not args.host or not re.fullmatch(r'[A-Za-z0-9_.@-]+',args.host) or args.host.startswith('-')):
        parser.error('Use an SSH host alias/user@host or --prepare-only')
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True);report_path=out/'report.json'
    if report_path.exists():parser.error('Use a fresh output directory to preserve existing evidence')
    prepared_report=None;prepared_report_bytes=None;prepared_archive_bytes=None
    if args.prepared_from:
        prepared_dir=args.prepared_from.resolve()
        prepared_report_bytes=(prepared_dir/'report.json').read_bytes()
        prepared_report=json.loads(prepared_report_bytes)
        prepared_archive_bytes=(prepared_dir/'inputs.tar.gz').read_bytes()
        if prepared_report.get('remote_job_started') or prepared_report.get('remote_stage'):
            raise RuntimeError('Prepared inputs must come from a local preparation with no remote job')
        if len(prepared_archive_bytes)!=prepared_report['archive_bytes'] or digest(prepared_archive_bytes)!=prepared_report['archive_sha256']:
            raise RuntimeError('Prepared archive hash/size mismatch')
        data={};paths={}
        with tarfile.open(fileobj=io.BytesIO(prepared_archive_bytes),mode='r:gz') as archive:
            members=archive.getmembers();expected=prepared_report['source_files']
            if len(members)!=len(expected) or {m.name for m in members}!=set(expected):
                raise RuntimeError('Prepared archive members mismatch')
            if sum(m.size for m in members)>80*1024*1024:raise RuntimeError('Prepared archive exceeds input bound')
            for member in members:
                parts=member.name.split('/')
                if not member.isfile() or member.size!=expected[member.name]['bytes'] or any(part in {'','.','..'} for part in parts) or any('\\' in part or ':' in part for part in parts):
                    raise RuntimeError('Prepared archive member type/size/path invalid')
                value=archive.extractfile(member).read()
                if digest(value)!=expected[member.name]['sha256']:raise RuntimeError('Prepared member hash mismatch')
                data[member.name]=value
        extend_shader=bool(prepared_report.get('extend_shader'))
        extend_pipeline=bool(prepared_report.get('extend_pipeline'))
        extend_command=bool(prepared_report.get('extend_command'))
        extension=extend_shader or extend_pipeline or extend_command
        pipeline_layer=extend_pipeline or extend_command
        prior=json.loads(data['provenance/prior-runtime-report.json']) if prepared_report.get('relink_only') or extension else None
    else:
        extend_shader=bool(args.extend_shader_from)
        extend_pipeline=bool(args.extend_pipeline_from)
        extend_command=bool(args.extend_command_from)
        extension=extend_shader or extend_pipeline or extend_command
        pipeline_layer=extend_pipeline or extend_command
        compiler_report_path=args.compiler_report.resolve() if args.compiler_report else ROOT/'build/metal-metallib/native-run-2/report.json'
        compiler_report_bytes=compiler_report_path.read_bytes();compiler_report=json.loads(compiler_report_bytes)
        if not compiler_report['success'] or not compiler_report['artifacts_exported']:
            raise RuntimeError('Existing native compiler validation is not successful')
        for name,expected in compiler_report['compiler_source_sha256'].items():
            if digest((ROOT/name).read_bytes())!=expected:raise RuntimeError('Existing compiler source is stale: '+name)
        aco_report_path=ROOT/'build/gfx11-compiler/darwin-build-report.json'
        aco_report_bytes=aco_report_path.read_bytes();aco_report=json.loads(aco_report_bytes)
        for name,expected in aco_report['adapter_sources'].items():
            if digest((ROOT/'userspace/compiler'/name).read_bytes())!=expected:raise RuntimeError('ACO adapter source stale: '+name)
        paths={
            'libhackynavi_metal_compiler.dylib':compiler_report_path.parent/'libhackynavi_metal_compiler.dylib',
            'apple-air-dis':compiler_report_path.parent/'apple-air-dis',
            'hackynavi-aco':Path(aco_report['binary']),
            'spirv-val':ROOT/'.tools/userspace/build-spirv-tools-macos/tools/spirv-val',
            'include/hackynavi/native_abi.hpp':ROOT/'include/hackynavi/native_abi.hpp',
            'userspace/transport/iokit_client.hpp':ROOT/'userspace/transport/iokit_client.hpp',
            'userspace/transport/iokit_client.cpp':ROOT/'userspace/transport/iokit_client.cpp',
            'userspace/transport/native_protocol.cpp':ROOT/'userspace/transport/native_protocol.cpp',
        }
        runtime_names=['compute_context.hpp','compute_context.cpp','pipeline_snapshot.cpp','context-smoke.cpp','build-native.py',
                       'tests/pipeline_snapshot_tests.cpp','tests/run_native.py','tests/fixtures/affine_xor_reordered.ll']
        if extension:runtime_names+=['compute_shader.hpp','compute_shader.cpp']
        if extend_shader:runtime_names+=['tests/compute_shader_tests.cpp']
        if extend_pipeline:runtime_names+=['tests/compute_pipeline_admission_tests.cpp']
        if extend_command:runtime_names+=['command_lifecycle.hpp','command_lifecycle.cpp','tests/command_lifecycle_tests.cpp']
        for name in runtime_names:
            paths['userspace/runtime/'+name]=ROOT/'userspace/runtime'/name
        for name in ['HNMetalBuffer.hpp','HNMetalBuffer.mm']+(['HNMetalLibrary.hpp','HNMetalLibrary.mm'] if extension else [])+(['HNMetalComputePipeline.hpp','HNMetalComputePipeline.mm'] if pipeline_layer else [])+(['HNMetalCommand.hpp','HNMetalCommand.mm'] if extend_command else []):
            paths['userspace/metal/native/'+name]=ROOT/'userspace/metal/native'/name
        compiler_headers=['hackynavi_shader.h','hackynavi_dispatch.h','hackynavi_pipeline.h']+(['hackynavi_metallib.h'] if extension else [])
        for name in compiler_headers:
            paths['userspace/metal/include/'+name]=ROOT/'userspace/metal/include'/name
        for name in ['apple/add_one.air','apple/provenance.json','affine_xor.ll']+(['function_constant_partial.ll'] if extend_shader else []):
            paths['userspace/metal/tests/fixtures/'+name]=ROOT/'userspace/metal/tests/fixtures'/name
        if pipeline_layer:
            for name in ['include/hackynavi/gfx11_occupancy.hpp','include/hackynavi/gfx11_compute_product.hpp','src/gfx11_compute_product.cpp']:
                paths[name]=ROOT/name
        if extend_pipeline:
            for name in ['apple-32023-macos15-add-one.air','apple-32023-macos15-add-one.metallib','apple-32023-macos15-provenance.json']:
                paths['userspace/metal/tests/fixtures/metallib/'+name]=ROOT/'userspace/metal/tests/fixtures/metallib'/name
        data={name:path.read_bytes() for name,path in paths.items()}
        for name,key in [('libhackynavi_metal_compiler.dylib','dylib'),('apple-air-dis','apple_air_dis')]:
            if digest(data[name])!=compiler_report['native'][key]['sha256']:raise RuntimeError('Exported compiler tool hash mismatch: '+name)
        verified_fixtures=['apple/add_one.air','affine_xor.ll']
        if extend_pipeline:
            verified_fixtures+=['metallib/apple-32023-macos15-add-one.air',
                'metallib/apple-32023-macos15-add-one.metallib','metallib/apple-32023-macos15-provenance.json']
        for name in verified_fixtures:
            if digest(data['userspace/metal/tests/fixtures/'+name])!=compiler_report['source_files']['tests/fixtures/'+name]['sha256']:
                raise RuntimeError('Compiler fixture differs from existing native evidence: '+name)
        if digest(data['hackynavi-aco'])!=aco_report['binary_sha256']:raise RuntimeError('ACO binary hash mismatch')
        if digest(data['spirv-val'])!=compiler_report['source_files']['spirv-val']['sha256']:raise RuntimeError('SPIR-V validator hash mismatch')
        for name in compiler_headers:
            if digest(data['userspace/metal/include/'+name])!=compiler_report['source_files']['include/'+name]['sha256']:
                raise RuntimeError('Compiler C ABI header differs from verified native export: '+name)
        data['provenance/compiler-native-report.json']=compiler_report_bytes
        data['provenance/aco-build-report.json']=aco_report_bytes
        prior=None
        if args.relink_from:
            prior_dir=args.relink_from.resolve();prior_bytes=(prior_dir/'report.json').read_bytes();prior=json.loads(prior_bytes)
            if not (prior.get('success') and prior.get('artifacts_exported') and prior.get('remote_stage_removed')):
                raise RuntimeError('Relink requires a verified successful prior runtime build')
            if prior.get('relink_only') or not prior['native'].get('offline_tests',{}).get('success'):
                raise RuntimeError('Relink base must include actual successful offline compiler tests')
            allowed_changes={'userspace/metal/native/HNMetalBuffer.hpp','userspace/metal/native/HNMetalBuffer.mm',
                             'userspace/runtime/build-native.py','userspace/runtime/tests/run_native.py'}
            for name,value in data.items():
                if name not in allowed_changes and prior['source_files'].get(name,{}).get('sha256')!=digest(value):
                    raise RuntimeError('Relink input differs from tested base: '+name)
            old_archive=(prior_dir/'libHackyNaviComputeRuntime.a').read_bytes()
            if digest(old_archive)!=prior['native']['artifacts']['libHackyNaviComputeRuntime.a']['sha256']:
                raise RuntimeError('Prior runtime archive hash mismatch')
            # Reuse only authenticated production objects; no compiler helper or
            # shader fixture is executed by the focused ObjC relink.
            for name in list(data):
                if name in {'apple-air-dis','hackynavi-aco','spirv-val'} or '/tests/fixtures/' in name:
                    del data[name];paths.pop(name,None)
            data['prior-runtime.a']=old_archive
            data['provenance/prior-runtime-report.json']=prior_bytes
        if extension:
            prior_dir=(args.extend_command_from or args.extend_pipeline_from or args.extend_shader_from).resolve();prior_bytes=(prior_dir/'report.json').read_bytes();prior=json.loads(prior_bytes)
            if not (prior.get('success') and prior.get('artifacts_exported') and prior.get('remote_stage_removed')):
                raise RuntimeError('Runtime extension requires a verified successful native runtime archive')
            if extend_pipeline and not prior['native'].get('shader_tests',{}).get('success'):
                raise RuntimeError('Pipeline extension requires actual successful Shader tests in its base')
            if extend_command and not prior['native'].get('admission_tests',{}).get('success'):
                raise RuntimeError('Command extension requires actual successful compiler admission tests in its base')
            # These are exactly the source dependencies of the reused objects;
            # the pipeline mode additionally authenticates Shader and Library.
            unchanged=['include/hackynavi/native_abi.hpp','userspace/transport/iokit_client.hpp',
                'userspace/transport/iokit_client.cpp','userspace/transport/native_protocol.cpp',
                'userspace/runtime/compute_context.hpp','userspace/runtime/compute_context.cpp','userspace/runtime/pipeline_snapshot.cpp',
                'userspace/metal/native/HNMetalBuffer.hpp','userspace/metal/native/HNMetalBuffer.mm',
                *['userspace/metal/include/'+name for name in ['hackynavi_shader.h','hackynavi_dispatch.h','hackynavi_pipeline.h']]]
            if pipeline_layer:
                unchanged+=['userspace/runtime/compute_shader.hpp','userspace/runtime/compute_shader.cpp',
                    'userspace/metal/native/HNMetalLibrary.hpp','userspace/metal/native/HNMetalLibrary.mm',
                    'userspace/metal/include/hackynavi_metallib.h']
            if extend_command:
                unchanged+=['userspace/metal/native/HNMetalComputePipeline.hpp','userspace/metal/native/HNMetalComputePipeline.mm',
                    'src/gfx11_compute_product.cpp','include/hackynavi/gfx11_compute_product.hpp','include/hackynavi/gfx11_occupancy.hpp']
            for name in unchanged:
                if digest(data[name])!=prior['source_files'].get(name,{}).get('sha256'):
                    raise RuntimeError('Reused runtime object input changed: '+name)
            old_archive=(prior_dir/'libHackyNaviComputeRuntime.a').read_bytes()
            if digest(old_archive)!=prior['native']['artifacts']['libHackyNaviComputeRuntime.a']['sha256']:
                raise RuntimeError('Prior runtime archive hash mismatch')
            expected_exports=['hn_metallib_abi_version','hn_metallib_parse','hn_metallib_error','hn_metallib_function_count',
                              'hn_metallib_function_info','hn_metallib_copy_name','hn_metallib_copy_air','hn_metallib_free']
            # Exact current export names are recorded by the compiler's native
            # validation. Require the owned library surface this layer calls.
            if not all(name in compiler_report['exports'] for name in expected_exports):
                raise RuntimeError('Compiler report lacks required owned metallib ABI exports')
            data['prior-runtime.a']=old_archive
            data['provenance/prior-runtime-report.json']=prior_bytes
            data['provenance/runtime-extension.json']=json.dumps({'mode':'command-objects' if extend_command else 'compute-pipeline' if extend_pipeline else 'shader-library'}).encode()
        if extend_command:
            # Command metadata tests do not invoke the actual compiler. Retain
            # its verified dylib for real runtime linkage, but stage no helper
            # executables or shader fixtures and rerun no compiler suites.
            for name in list(data):
                if name in {'apple-air-dis','hackynavi-aco','spirv-val'} or '/tests/fixtures/' in name:
                    del data[name];paths.pop(name,None)
    source_bytes=sum(map(len,data.values()))
    if source_bytes>80*1024*1024:raise RuntimeError('Runtime inputs exceed80MiB bound')
    source=out/'inputs.tar.gz'
    executable={'apple-air-dis','hackynavi-aco','spirv-val'}
    if prepared_archive_bytes is not None:source.write_bytes(prepared_archive_bytes)
    else:
        with tarfile.open(source,'w:gz') as archive:
            for name,value in data.items():
                member=tarfile.TarInfo(name);member.size=len(value);member.mode=0o755 if name in executable else 0o644
                archive.addfile(member,io.BytesIO(value))
    hashes={name:{'bytes':len(value),'sha256':digest(value)} for name,value in data.items()}
    report={'success':False,'source_files':hashes,'source_bytes':source_bytes,'archive_sha256':digest(source.read_bytes()),
            'local_orchestrator_sha256':digest(Path(__file__).read_bytes()),
            'archive_bytes':source.stat().st_size,'gpu_executed':False,'context_open_called':False,'smoke_executed':False,
            'remote_job_started':False,'remote_job_finished':False,'remote_stage_removed':False,'artifacts_exported':False,
            'relink_only':bool(prior) and not extension,'extend_shader':extend_shader,'extend_pipeline':extend_pipeline,'extend_command':extend_command,'offline_tests_rerun':not bool(prior)}
    if prepared_report is not None:
        report['prepared_input_report_sha256']=digest(prepared_report_bytes)
        report['input_snapshot']='previously prepared immutable archive'
    def persist():report_path.write_text(json.dumps(report,indent=2)+'\n')
    persist();print(f'Prepared {source_bytes:,} bytes, {source.stat().st_size:,} compressed',flush=True)
    if args.prepare_only:return 0
    options=['-o','BatchMode=yes','-o','ConnectTimeout=10']+(['-o','IdentityAgent=none'] if args.no_agent else [])
    ssh=['ssh',*options,args.host];scp=['scp','-q',*options]
    remote=None;terminal=False;build_success=False
    try:
        setup=('set -eu; test "$(uname -s)" = Darwin; test "$(uname -m)" = x86_64; '
               'avail=$(df -Pk "$HOME" | tail -1 | awk \'{print $4}\'); test "$avail" -ge 1048576; '
               'mktemp -d "$HOME/Library/Caches/HackyNavi-ComputeRuntime.XXXXXX"')
        remote=subprocess.check_output(ssh+[setup],text=True,timeout=30).strip()
        if not re.fullmatch(r'/Users/[^/\r\n]+/Library/Caches/HackyNavi-ComputeRuntime\.[A-Za-z0-9]+',remote):
            remote=None;raise RuntimeError('Unexpected remote stage path')
        report['remote_stage']=remote;persist()
        # Whitelist and verify every archive member before any compiler executes.
        bootstrap='''import hashlib,json,subprocess,sys,tarfile
from pathlib import Path
stage=Path(sys.argv[1]).resolve()
if str(stage)!=STAGE:raise RuntimeError('stage mismatch')
archive=stage/'inputs.tar.gz'
if hashlib.sha256(archive.read_bytes()).hexdigest()!=ARCHIVE:raise RuntimeError('archive hash mismatch')
with tarfile.open(archive,'r:gz') as tar:
 members=tar.getmembers()
 if len(members)!=len(FILES) or {m.name for m in members}!=set(FILES):raise RuntimeError('archive members mismatch')
 for member in members:
  if not member.isfile() or member.size!=FILES[member.name]['bytes']:raise RuntimeError('archive member type/size')
  data=tar.extractfile(member).read()
  if hashlib.sha256(data).hexdigest()!=FILES[member.name]['sha256']:raise RuntimeError('member hash mismatch')
  path=(stage/member.name).resolve()
  if not path.is_relative_to(stage):raise RuntimeError('member outside stage')
  path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data);path.chmod(0o755 if member.name in EXECUTABLE else 0o644)
raise SystemExit(subprocess.call([sys.executable,str(stage/'userspace/runtime/tests/run_native.py'),str(stage)]))
'''
        bootstrap='STAGE='+repr(remote)+'\nARCHIVE='+repr(report['archive_sha256'])+'\nFILES='+repr(hashes)+'\nEXECUTABLE='+repr(executable)+'\n'+bootstrap
        bootstrap_path=out/'bootstrap.py';bootstrap_path.write_text(bootstrap)
        subprocess.run(scp+[str(source),str(bootstrap_path),args.host+':'+remote+'/'],check=True,timeout=60)
        report['remote_job_started']=True;persist()
        result=subprocess.run(ssh+['python3 '+shlex.quote(remote+'/bootstrap.py')+' '+shlex.quote(remote)],
                              capture_output=True,text=True,timeout=600)
        report['ssh_exit_code']=result.returncode
        (out/'bootstrap.log').write_text(result.stdout+result.stderr)
        # A shell exit alone is insufficient: require the runner's process-group
        # report before deleting a stage that could have a surviving compiler.
        native_result=subprocess.run(ssh+['cat '+shlex.quote(remote+'/native-report.json')],capture_output=True,text=True,timeout=30)
        if native_result.returncode:raise RuntimeError('No terminal native process report; preserve stage')
        native=json.loads(native_result.stdout);report['native']=native
        terminal=bool(native.get('complete') and native.get('child_processes_reaped') and native.get('stage')==remote)
        report['remote_job_finished']=terminal;build_success=bool(terminal and native.get('success') and result.returncode==0)
        persist()
        if not terminal:raise RuntimeError('Native descendants not confirmed stopped; preserve stage')
        if not build_success:raise RuntimeError('Native runtime build/tests failed; inspect native report')
        archive=out/'artifacts.tar.gz'
        subprocess.run(scp+[args.host+':'+remote+'/artifacts.tar.gz',str(archive)],check=True,timeout=60)
        if digest(archive.read_bytes())!=native['artifact_archive_sha256']:raise RuntimeError('Artifact archive hash mismatch')
        with tarfile.open(archive,'r:gz') as tar:
            members=tar.getmembers();expected=native['artifacts']
            allowed={'libHackyNaviComputeRuntime.a','libHackyNaviComputeRuntime.dylib','hackynavi-context-smoke','MTLAllocation.h'}
            if not prior:allowed.update({'pipeline-snapshot-tests','offline-tests.json'})
            if extend_shader:allowed.update({'compute-shader-tests','shader-tests.json'})
            if extend_pipeline:allowed.update({'compute-pipeline-admission-tests','admission-tests.json'})
            if extend_command:allowed.update({'command-lifecycle-tests','command-tests.json'})
            if set(expected)!=allowed or len(members)!=len(allowed) or {m.name for m in members}!=allowed:
                raise RuntimeError('Unexpected artifact members')
            if sum(m.size for m in members)>32*1024*1024:raise RuntimeError('Unexpected artifact size')
            for member in members:
                if not member.isfile() or member.size!=expected[member.name]['bytes']:raise RuntimeError('Invalid artifact member')
                value=tar.extractfile(member).read()
                if digest(value)!=expected[member.name]['sha256']:raise RuntimeError('Artifact member hash mismatch')
                (out/member.name).write_bytes(value)
        report['artifacts_exported']=True;persist()
    except (OSError,ValueError,RuntimeError,subprocess.SubprocessError,tarfile.TarError) as error:
        report['error']=str(error)
    finally:
        if remote and (not report['remote_job_started'] or (terminal and (not build_success or report['artifacts_exported']))):
            try:
                quoted=shlex.quote(remote)
                cleanup=subprocess.run(ssh+['rm -rf -- '+quoted+'; test ! -e '+quoted],capture_output=True,text=True,timeout=30)
                report['remote_stage_removed']=cleanup.returncode==0
            except (OSError,subprocess.SubprocessError) as error:report['cleanup_error']=str(error)
        elif remote:report['remote_job_state']='preserved; terminal process status or verified export is incomplete'
        report['snapshot_matches_worktree']=None if prepared_report is not None else all(path.read_bytes()==data[name] for name,path in paths.items())
        report['snapshot_verified']=digest(source.read_bytes())==report['archive_sha256'] and (prepared_report is not None or report['snapshot_matches_worktree'])
        report['success']=bool(build_success and report['artifacts_exported'] and report['remote_stage_removed'] and report['snapshot_verified'])
        persist()
        print(json.dumps({'success':report['success'],'remote_stage_removed':report['remote_stage_removed'],
                          'checks':report.get('native',{}).get('command_tests',report.get('native',{}).get('admission_tests',report.get('native',{}).get('shader_tests',report.get('native',{}).get('offline_tests',{})))).get('checks'),'error':report.get('error')}),flush=True)
    return 0 if report['success'] else 1
if __name__=='__main__':raise SystemExit(main())
