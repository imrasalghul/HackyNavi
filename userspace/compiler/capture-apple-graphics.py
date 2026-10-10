#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded offline Apple AIR/metallib capture; no Metal objects or GPU access.

Uses the already installed measured Metal toolchain. No downloads/installation,
no shared source stage, and no cleanup until terminal output is exported and
hash-verified. An uncertain SSH outcome retains the pre-journaled unique stage.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import subprocess
import uuid

ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/'userspace/compiler/fixtures/apple-graphics/triangle.metal'
FILES=('graphics.metal','graphics.air','graphics.metallib')


def digest(data): return hashlib.sha256(data).hexdigest()


REMOTE=r'''
import base64,hashlib,json,os,pathlib,resource,shutil,signal,subprocess
source=base64.b64decode(SOURCE_BASE64,validate=True)
cache=pathlib.Path.home()/'Library/Caches'
stage=cache/STAGE_NAME
if stage.parent!=cache or stage.exists() or stage.is_symlink(): raise RuntimeError('Stage is not new')
if shutil.disk_usage(cache).free<32*1024*1024: raise RuntimeError('Insufficient 32MiB storage margin')
stage.mkdir(mode=0o700)
(stage/'graphics.metal').write_bytes(source)
r={'stage':str(stage),'commands':[],'artifacts':{},'error':None,'terminal':False}
print(json.dumps({'stage':str(stage)}),flush=True)
def limited(): resource.setrlimit(resource.RLIMIT_FSIZE,(4*1024*1024,4*1024*1024))
def run(args):
    env=os.environ.copy();env['TMPDIR']=str(stage)+'/'
    p=subprocess.Popen(args,cwd=stage,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,start_new_session=True,preexec_fn=limited)
    try: stdout,stderr=p.communicate(timeout=60)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid,signal.SIGKILL);stdout,stderr=p.communicate()
        raise RuntimeError('Tool timeout; complete process group terminated')
    if len(stdout)+len(stderr)>256*1024: raise RuntimeError('Unexpected tool output size')
    r['commands'].append({'argv':args,'exit_code':p.returncode,'stdout':stdout.decode('utf-8','replace'),'stderr':stderr.decode('utf-8','replace')})
    if p.returncode: raise RuntimeError('Compiler tool failed')
try:
    prefix=['xcrun','--sdk','macosx','--toolchain','com.apple.dt.toolchain.Metal.32023.864']
    run(['sw_vers']);run(prefix+['metal','--version'])
    run(prefix+['metal','-std=macos-metal2.4','-mmacosx-version-min=15.0','-c','graphics.metal','-o','graphics.air'])
    run(prefix+['metallib','graphics.air','-o','graphics.metallib'])
except BaseException as error: r['error']=str(error)
try:
    for name in ['graphics.metal','graphics.air','graphics.metallib']:
        path=stage/name
        if path.is_file():
            if path.is_symlink() or path.stat().st_size>1024*1024: raise RuntimeError('Artifact exceeds capture bounds')
            data=path.read_bytes()
            if len(data)>1024*1024: raise RuntimeError('Artifact grew beyond capture bounds')
            r['artifacts'][name]={'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),'base64':base64.b64encode(data).decode()}
    r['stage_bytes']=sum(p.stat().st_size for p in stage.rglob('*') if p.is_file())
    if r['stage_bytes']>8*1024*1024: raise RuntimeError('Stage exceeds 8MiB bound')
except BaseException as error: r['error']=str(error)
r['terminal']=True
print(json.dumps(r),flush=True)
'''


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--prepare-only',action='store_true')
    args=parser.parse_args();out=args.output.resolve()
    if out.exists(): parser.error('Output must be new; previous evidence is immutable')
    source=SOURCE.read_bytes()
    if not source or len(source)>64*1024: parser.error('Source exceeds capture bounds')
    name='HackyNavi-AppleGraphics-'+uuid.uuid4().hex
    stage='/Users/victor/Library/Caches/'+name
    remote=REMOTE.replace('SOURCE_BASE64',repr(base64.b64encode(source).decode())).replace('STAGE_NAME',repr(name))
    out.mkdir(parents=True);(out/'graphics.metal').write_bytes(source);(out/'remote.py').write_text(remote)
    report={'success':False,'prepared_only':args.prepare_only,'stage':stage,'stage_removed':False,
            'source_path':SOURCE.relative_to(ROOT).as_posix(),'source_sha256':digest(source),'source_bytes':len(source),
            'capture_runner_sha256':digest(Path(__file__).read_bytes()),'remote_script_sha256':digest(remote.encode()),
            'gpu_execution':False,'metal_objects_created':False,'installation_performed':False}
    def save(): (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    save()
    if args.prepare_only:
        print(json.dumps({'prepared':True,'report':str(out/'report.json')}));return
    ssh=['ssh','-o','BatchMode=yes','-o','ConnectTimeout=10','-o','IdentityAgent=none','victor@mini','python3 -']
    # Persist stdout while the remote job runs, preserving its stage announcement
    # even if the local caller or SSH connection is interrupted.
    with (out/'remote-output.jsonl').open('wb') as stdout,(out/'remote-stderr.txt').open('wb') as stderr:
        result=subprocess.run(ssh,input=remote.encode(),stdout=stdout,stderr=stderr)
    report['ssh_exit_code']=result.returncode;save()
    if result.returncode: raise SystemExit('Remote outcome unconfirmed; preserve journal/stage')
    transport=out/'remote-output.jsonl'
    if transport.stat().st_size>5*1024*1024: raise SystemExit('Unexpected transport size; preserve stage')
    lines=transport.read_text().splitlines();data=json.loads(lines[-1])
    if data.get('stage')!=stage or data.get('terminal') is not True: raise SystemExit('Remote completion not confirmed')
    artifacts=data.pop('artifacts');report.update(data);save()
    for name,item in artifacts.items():
        if name not in FILES or type(item.get('bytes')) is not int or not 0<=item['bytes']<=1024*1024:
            raise SystemExit('Unexpected artifact; preserve stage')
        raw=base64.b64decode(item.pop('base64'),validate=True)
        if len(raw)!=item['bytes'] or digest(raw)!=item['sha256']: raise SystemExit('Export hash mismatch; preserve stage')
        if name=='graphics.metal':
            if raw!=source: raise SystemExit('Source changed on remote host')
        else: (out/name).write_bytes(raw)
    report['artifacts']=artifacts;report['export_hashes_verified']=True;save()
    if not re.fullmatch(r'/Users/victor/Library/Caches/HackyNavi-AppleGraphics-[0-9a-f]{32}',stage):
        raise SystemExit('Invalid cleanup stage')
    cleanup='import pathlib,shutil; p=pathlib.Path('+repr(stage)+'); assert p.parent==pathlib.Path.home()/"Library/Caches"; assert not p.is_symlink(); shutil.rmtree(p); assert not p.exists(); print("removed")'
    result=subprocess.run(ssh,input=cleanup.encode(),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    report.update(cleanup_exit_code=result.returncode,cleanup_stdout=result.stdout.decode(),cleanup_stderr=result.stderr.decode())
    report['stage_removed']=result.returncode==0 and result.stdout.strip()==b'removed'
    report['success']=report['stage_removed'] and not report['error'] and set(artifacts)==set(FILES)
    report['source_unchanged']=SOURCE.read_bytes()==source
    report['success'] &= report['source_unchanged'];save()
    print(json.dumps({'success':report['success'],'stage_removed':report['stage_removed'],'stage_bytes':report.get('stage_bytes'),
                      'report_sha256':digest((out/'report.json').read_bytes())}))
    if not report['success']: raise SystemExit(1)


if __name__=='__main__':main()
