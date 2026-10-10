#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Freeze, compile, verify and remove a bounded native frontend build stage."""
import argparse,hashlib,io,json,re,shlex,subprocess,tarfile
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
SOURCES=['userspace/runtime/'+n+'.cpp' for n in ('pipeline_snapshot','compute_context','compute_shader','command_lifecycle',
    'graphics_snapshot','graphics_buffer_snapshot','graphics_shader','graphics_build','texture_region')]+[
    'userspace/transport/native_protocol.cpp','userspace/transport/iokit_client.cpp','src/gfx11_compute_product.cpp',
    'src/gfx11_graphics_product.cpp']+[
    'userspace/metal/native/'+n+'.mm' for n in ('HNMetalBuffer','HNMetalLibrary','HNMetalComputePipeline',
        'HNMetalCommand','HNMetalDevice','HNMetalCapabilities','HNMetalArchitecture','HNMetalTexture','HNMetalRenderPipeline')]
DEVICE_SOURCES={'userspace/metal/native/'+n+'.mm' for n in ('HNMetalDevice','HNMetalCapabilities','HNMetalArchitecture')}
RUNNER='userspace/runtime/tests/build_frontend_native.py'
EXPORTS={'libHackyNaviMetalFrontend.a','libHackyNaviMetalFrontend.dylib','hackynavi-metal-compute-smoke'}
def sha(data): return hashlib.sha256(data).hexdigest()
def snapshot(sources):
    pending=sources+[RUNNER,'userspace/runtime/build-frontend.py','userspace/metal/native/compute-smoke.mm',
        'userspace/metal/native/render-smoke.mm', 'userspace/metal/native/render-buffer-smoke.mm',
        'userspace/metal/native/render-vertex-buffer-smoke.mm'];data={}
    while pending:
        name=pending.pop()
        if name in data: continue
        path=ROOT/name;value=path.read_bytes();data[name]=value
        for delimiter,include in re.findall(r'^\s*#(?:include|import)\s+(["<])([^">]+)[">]',value.decode(),re.M):
            if delimiter=='<' and not include.startswith('hackynavi/'): continue
            found=next((p for p in (path.parent/include,ROOT/'include'/include) if p.is_file()),None)
            if found is None: raise RuntimeError('Missing local include: '+include)
            pending.append(found.resolve().relative_to(ROOT).as_posix())
    return data
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host',default='victor@mini');parser.add_argument('--no-agent',action='store_true')
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--prepare-only',action='store_true');parser.add_argument('--prepared-from',type=Path)
    parser.add_argument('--compiler-report',type=Path)
    parser.add_argument('--graphics-report',type=Path)
    parser.add_argument('--components-only',action='store_true',help='Compile the real buffer/library/pipeline/command objects without the pending device/capability layer')
    args=parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_.@-]+',args.host) or args.host.startswith('-'): parser.error('Invalid SSH host')
    if args.prepared_from and (args.prepare_only or args.compiler_report or args.graphics_report or args.components_only): parser.error('Prepared input cannot be combined with fresh preparation')
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
    if any(output.iterdir()): parser.error('Choose a fresh empty evidence directory; existing inputs/artifacts are never overwritten')
    if args.prepared_from:
        prior_bytes=(args.prepared_from/'report.json').read_bytes();prior=json.loads(prior_bytes)
        archive_bytes=(args.prepared_from/'inputs.tar.gz').read_bytes();data={}
        if prior['remote_job_started'] or sha(archive_bytes)!=prior['archive_sha256']: raise RuntimeError('Invalid preparation')
        with tarfile.open(fileobj=io.BytesIO(archive_bytes),mode='r:gz') as tar:
            members=tar.getmembers();expected=prior['source_files']
            if len(members)!=len(expected) or {m.name for m in members}!=set(expected): raise RuntimeError('Input member mismatch')
            if sum(m.size for m in members)>64*1024*1024: raise RuntimeError('Input size exceeded')
            for m in members:
                if not m.isfile() or m.size!=expected[m.name]['bytes']: raise RuntimeError('Input member type/size')
                value=tar.extractfile(m).read()
                if sha(value)!=expected[m.name]['sha256']: raise RuntimeError('Input hash mismatch')
                data[m.name]=value
    else:
        if not args.compiler_report or not args.graphics_report: parser.error('Fresh preparation requires --compiler-report and --graphics-report')
        sources=[name for name in SOURCES if name not in DEVICE_SOURCES] if args.components_only else SOURCES
        data=snapshot(sources);p=args.compiler_report.resolve();compiler_bytes=p.read_bytes();compiler=json.loads(compiler_bytes)
        if not compiler['success'] or not compiler['artifacts_exported']: raise RuntimeError('Compiler validation is unsuccessful')
        value=(p.parent/'libhackynavi_metal_compiler.dylib').read_bytes()
        if sha(value)!=compiler['native']['dylib']['sha256']: raise RuntimeError('Compiler dylib differs from report')
        for name,entry in compiler['source_files'].items():
            if name.startswith('include/'):
                local='userspace/metal/'+name
                if local in data and sha(data[local])!=entry['sha256']: raise RuntimeError('Compiler header mismatch: '+name)
        data['libhackynavi_metal_compiler.dylib']=value
        data['provenance/compiler-report.json']=compiler_bytes
        gp=args.graphics_report.resolve();graphics_bytes=gp.read_bytes();graphics=json.loads(graphics_bytes)
        if not all(graphics.get(k) for k in ('success','native_tests_passed','artifacts_exported','remote_job_finished','remote_stage_removed')):
            raise RuntimeError('Native graphics compiler/layout evidence incomplete')
        if set(graphics['native']['exports']['libHackyNaviGraphicsCompiler.dylib']) != {
                '_hn_gfx11_ngg_compile_validated_file', '_hn_gfx11_fragment_compile_validated_file',
                '_hn_gfx11_fragment_buffer_compile_validated_file', '_hn_gfx11_fragment_buffer_descriptor',
                '_hn_gfx11_vertex_buffer_compile_validated_file', '_hn_gfx11_vertex_buffer_descriptor'}:
            raise RuntimeError('Native graphics buffer compiler exports differ from the actual frontend contract')
        for name in ('libHackyNaviGraphicsCompiler.dylib','libHackyNaviColorLayout.dylib'):
            value=(gp.parent/name).read_bytes();expected=graphics['native']['artifacts'][name]
            if len(value)!=expected['bytes'] or sha(value)!=expected['sha256']: raise RuntimeError('Native graphics artifact changed: '+name)
            data[name]=value
        data['provenance/graphics-report.json']=graphics_bytes
        data['build-plan.json']=json.dumps({'sources':sources,'device_layer':not args.components_only,'graphics':True},indent=2).encode()
        stream=io.BytesIO()
        with tarfile.open(fileobj=stream,mode='w:gz') as tar:
            for name,value in sorted(data.items()):
                member=tarfile.TarInfo(name);member.size=len(value);member.mode=0o644;tar.addfile(member,io.BytesIO(value))
        archive_bytes=stream.getvalue()
    for name in data:
        if any(p in {'','.','..'} or '\\' in p or ':' in p for p in name.split('/')): raise RuntimeError('Unsafe archive path')
    if sum(map(len,data.values()))>64*1024*1024: raise RuntimeError('Inputs exceed 64 MiB')
    (output/'inputs.tar.gz').write_bytes(archive_bytes)
    hashes={n:{'bytes':len(v),'sha256':sha(v)} for n,v in data.items()}
    report={'success':False,'source_files':hashes,'source_bytes':sum(map(len,data.values())),
            'archive_sha256':sha(archive_bytes),'archive_bytes':len(archive_bytes),'remote_job_started':False,
            'remote_job_finished':False,'remote_stage_removed':False,'artifacts_exported':False,
            'gpu_executed':False,'metal_instantiated':False,'context_open_called':False,'tests_executed':False}
    report['device_layer']=json.loads(data['build-plan.json'])['device_layer']
    report['graphics']=bool(json.loads(data['build-plan.json']).get('graphics'))
    if args.prepared_from: report['preparation_report_sha256']=sha(prior_bytes)
    def persist(): (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    persist();print('Frozen frontend input bytes:',report['source_bytes'],flush=True)
    if args.prepare_only: return 0
    opts=['-o','BatchMode=yes','-o','ConnectTimeout=10']+(['-o','IdentityAgent=none'] if args.no_agent else [])
    ssh=['ssh',*opts,args.host];scp=['scp','-q',*opts];remote=None;terminal=False;success=False
    try:
        setup='set -eu; test "$(uname -s)" = Darwin; test "$(uname -m)" = x86_64; test "$(df -Pk "$HOME" | tail -1 | awk \'{print $4}\')" -ge 1048576; mktemp -d "$HOME/Library/Caches/HackyNavi-Frontend.XXXXXX"'
        remote=subprocess.check_output(ssh+[setup],text=True,timeout=30).strip()
        if not re.fullmatch(r'/Users/[^/\r\n]+/Library/Caches/HackyNavi-Frontend\.[A-Za-z0-9]+',remote):
            remote=None;raise RuntimeError('Invalid remote stage')
        report['remote_stage']=remote;persist()
        bootstrap='STAGE='+repr(remote)+'\nHASH='+repr(sha(archive_bytes))+'\nFILES='+repr(hashes)+'\n'+'''
import hashlib,subprocess,sys,tarfile
from pathlib import Path
stage=Path(STAGE).resolve()
if str(stage)!=STAGE: raise RuntimeError('Stage mismatch')
archive=stage/'inputs.tar.gz'
if hashlib.sha256(archive.read_bytes()).hexdigest()!=HASH: raise RuntimeError('Archive hash mismatch')
with tarfile.open(archive,'r:gz') as tar:
 members=tar.getmembers()
 if len(members)!=len(FILES) or {m.name for m in members}!=set(FILES): raise RuntimeError('Input members mismatch')
 for m in members:
  if not m.isfile() or m.size!=FILES[m.name]['bytes']: raise RuntimeError('Member type/size mismatch')
  value=tar.extractfile(m).read()
  if hashlib.sha256(value).hexdigest()!=FILES[m.name]['sha256']: raise RuntimeError('Member hash mismatch')
  path=(stage/m.name).resolve()
  if not path.is_relative_to(stage): raise RuntimeError('Member escapes stage')
  path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(value)
raise SystemExit(subprocess.call([sys.executable,str(stage/'userspace/runtime/tests/build_frontend_native.py'),str(stage)]))
'''
        (output/'bootstrap.py').write_text(bootstrap)
        subprocess.run(scp+[str(output/'inputs.tar.gz'),str(output/'bootstrap.py'),args.host+':'+remote+'/'],check=True,timeout=60)
        report['remote_job_started']=True;persist()
        result=subprocess.run(ssh+['python3 '+shlex.quote(remote+'/bootstrap.py')],capture_output=True,text=True,timeout=600)
        (output/'bootstrap.log').write_text(result.stdout+result.stderr);report['ssh_exit_code']=result.returncode
        value=subprocess.check_output(ssh+['cat '+shlex.quote(remote+'/native-report.json')],text=True,timeout=30)
        native=json.loads(value);report['native']=native
        terminal=bool(native.get('complete') and native.get('child_processes_reaped') and native.get('stage')==remote)
        report['remote_job_finished']=terminal;success=bool(terminal and native.get('success') and not result.returncode);persist()
        if not terminal: raise RuntimeError('Unconfirmed process termination; preserve stage')
        if not success: raise RuntimeError('Native build failed; inspect preserved report')
        dest=output/'artifacts.tar.gz'
        subprocess.run(scp+[args.host+':'+remote+'/artifacts.tar.gz',str(dest)],check=True,timeout=60)
        if sha(dest.read_bytes())!=native['artifact_archive_sha256']: raise RuntimeError('Artifact archive hash mismatch')
        with tarfile.open(dest,'r:gz') as tar:
            members=tar.getmembers();expected=native['artifacts']
            wanted=EXPORTS|({'HackyNaviMetal'} if report['device_layer'] else set())
            if report['graphics']: wanted.update({'hackynavi-metal-render-smoke', 'hackynavi-metal-render-buffer-smoke',
                                                 'hackynavi-metal-render-vertex-buffer-smoke'})
            if set(expected)!=wanted or len(members)!=len(wanted) or {m.name for m in members}!=wanted: raise RuntimeError('Artifact members mismatch')
            if sum(m.size for m in members)>16*1024*1024: raise RuntimeError('Artifact size exceeded')
            for m in members:
                if not m.isfile() or m.size!=expected[m.name]['bytes']: raise RuntimeError('Artifact member type/size')
                value=tar.extractfile(m).read()
                if sha(value)!=expected[m.name]['sha256']: raise RuntimeError('Artifact member hash mismatch')
                (output/m.name).write_bytes(value)
        report['artifacts_exported']=True;persist()
    except (OSError,ValueError,RuntimeError,subprocess.SubprocessError,tarfile.TarError) as error: report['error']=str(error)
    finally:
        if remote and (not report['remote_job_started'] or terminal and (not success or report['artifacts_exported'])):
            try:
                p=shlex.quote(remote);cleanup=subprocess.run(ssh+['rm -rf -- '+p+'; test ! -e '+p],capture_output=True,text=True,timeout=30)
                report['remote_stage_removed']=cleanup.returncode==0
            except (OSError,subprocess.SubprocessError) as error: report['cleanup_error']=str(error)
        report['input_verified']=sha((output/'inputs.tar.gz').read_bytes())==report['archive_sha256']
        report['success']=bool(success and report['artifacts_exported'] and report['remote_stage_removed'] and report['input_verified'])
        persist();print(json.dumps({k:report.get(k) for k in ('success','remote_stage_removed','error')}),flush=True)
    return 0 if report['success'] else 1

if __name__=='__main__': raise SystemExit(main())
