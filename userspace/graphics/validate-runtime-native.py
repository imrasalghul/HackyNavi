#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run an immutable offline graphics runtime snapshot on the bounded Mac lane."""
import argparse,hashlib,io,json,re,shlex,subprocess,tarfile
from pathlib import Path

def sha(raw): return hashlib.sha256(raw).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--prepared',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
 p.add_argument('--host',default='victor@mini');p.add_argument('--no-agent',action='store_true')
 a=p.parse_args()
 if not re.fullmatch(r'[A-Za-z0-9_.@-]+',a.host) or a.host.startswith('-'):p.error('Invalid host')
 out=a.output.resolve()
 if out.exists():p.error('Use a fresh output directory')
 raw=(a.prepared/'inputs.tar.gz').read_bytes();prior=(a.prepared/'report.json').read_bytes();r=json.loads(prior)
 assert r['prepared'] and r['success'] and r.get('remote_execution_pending') and sha(raw)==r['archive_sha256']
 expected=r['source_files'];data={}
 with tarfile.open(fileobj=io.BytesIO(raw),mode='r:gz') as tar:
  members=tar.getmembers();assert len(members)==len(expected)+1
  assert {m.name for m in members}==set(expected)|{'manifest.json'}
  assert sum(m.size for m in members)<61*1024*1024
  for m in members:
   assert m.isfile() and all(part not in ('','.','..') and '\\' not in part and ':' not in part for part in m.name.split('/'))
   value=tar.extractfile(m).read()
   if m.name in expected:assert len(value)==expected[m.name]['bytes'] and sha(value)==expected[m.name]['sha256']
   data[m.name]={'sha256':sha(value),'bytes':len(value),'mode':m.mode&0o777}
 out.mkdir(parents=True);(out/'inputs.tar.gz').write_bytes(raw);(out/'preparation-report.json').write_bytes(prior)
 r.update(success=False,remote_executed=False,native_tests_passed=False,artifacts_exported=False,
          remote_job_started=False,remote_job_finished=False,remote_stage_removed=False,
          preparation_sha256=sha(prior),runner_sha256=sha(Path(__file__).read_bytes()))
 def save(): (out/'report.json').write_text(json.dumps(r,indent=2)+'\n',encoding='utf-8')
 opts=['-o','BatchMode=yes','-o','ConnectTimeout=10']+(['-o','IdentityAgent=none'] if a.no_agent else [])
 ssh=['ssh',*opts,a.host];scp=['scp','-q',*opts];stage=None;terminal=False
 save()
 try:
  stage=subprocess.check_output(ssh+['test "$(uname -sm)" = "Darwin x86_64" && test "$(df -Pk "$HOME" | tail -1 | awk \'{print $4}\')" -ge 1048576 && mktemp -d "$HOME/Library/Caches/HackyNavi-GraphicsRuntime.XXXXXX"'],text=True,timeout=30).strip()
  if not re.fullmatch(r'/Users/[^/\r\n]+/Library/Caches/HackyNavi-GraphicsRuntime\.[A-Za-z0-9]+',stage):stage=None;raise RuntimeError('Invalid stage')
  r['remote_stage']=stage;save()
  bootstrap='STAGE='+repr(stage)+'\nFILES='+repr(data)+'\nHASH='+repr(sha(raw))+'\n'+'''
import hashlib,subprocess,sys,tarfile
from pathlib import Path
s=Path(STAGE).resolve();assert str(s)==STAGE
a=s/'inputs.tar.gz';assert hashlib.sha256(a.read_bytes()).hexdigest()==HASH
with tarfile.open(a,'r:gz') as t:
 ms=t.getmembers();assert len(ms)==len(FILES) and {m.name for m in ms}==set(FILES)
 for m in ms:
  assert m.isfile() and m.size==FILES[m.name]['bytes']
  v=t.extractfile(m).read();assert hashlib.sha256(v).hexdigest()==FILES[m.name]['sha256']
  p=(s/m.name).resolve();assert p.is_relative_to(s)
  p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(v);p.chmod(FILES[m.name]['mode'])
raise SystemExit(subprocess.call([sys.executable,str(s/'userspace/graphics/run_runtime_native.py'),str(s)]))
'''
  (out/'bootstrap.py').write_text(bootstrap,encoding='utf-8')
  subprocess.run(scp+[str(out/'inputs.tar.gz'),str(out/'bootstrap.py'),a.host+':'+stage+'/'],check=True,timeout=60)
  r['remote_job_started']=True;r['remote_executed']=True;save()
  job=subprocess.run(ssh+['python3 '+shlex.quote(stage+'/bootstrap.py')],capture_output=True,text=True,timeout=600)
  (out/'native.log').write_text(job.stdout+job.stderr,encoding='utf-8');r['exit_code']=job.returncode
  n=json.loads(subprocess.check_output(ssh+['cat '+shlex.quote(stage+'/native-report.json')],timeout=30));r['native']=n
  terminal=bool(n['complete'] and n['child_processes_reaped'] and n['stage']==stage);r['remote_job_finished']=terminal;save()
  if not terminal:raise RuntimeError('Native child termination unconfirmed; preserve stage')
  if job.returncode or not n['success']:raise RuntimeError('Actual native graphics validation failed')
  r['native_tests_passed']=True
  names={'results/graphics_shader.o','results/graphics_build.o','results/graphics_snapshot.o',
         'results/graphics_buffer_snapshot.o',
         'results/gfx11_graphics_product.o','results/graphics_build_native.o','results/graphics-runtime-tests'}
  assert set(n['artifacts'])==names and n['inputs_unchanged'] and n['peak_stage_bytes']<=96*1024*1024
  assert n['gpu_executed'] is False and n['metal_objects_created'] is False and n['context_opened'] is False
  assert '96 checks, 0 failures, 8 actual compiler-backed graphics pipelines;' in n['test_output']
  for name,item in n['artifacts'].items():
   assert 0<item['bytes']<16*1024*1024 and re.fullmatch('[a-f0-9]{64}',item['sha256'])
   target=out/name;target.parent.mkdir(parents=True,exist_ok=True)
   subprocess.run(scp+[a.host+':'+stage+'/'+name,str(target)],check=True,timeout=60)
   exported=target.read_bytes();assert len(exported)==item['bytes'] and sha(exported)==item['sha256']
  r['exports']={name:sha((out/name).read_bytes()) for name in sorted(names)}
  r['artifacts_exported']=True;save()
 except (OSError,ValueError,AssertionError,RuntimeError,subprocess.SubprocessError) as e:r['error']=str(e)
 finally:
  if stage and (not r['remote_job_started'] or terminal and (not r.get('native_tests_passed') or r['artifacts_exported'])):
   cleanup=subprocess.run(ssh+['rm -rf -- '+shlex.quote(stage)+'; test ! -e '+shlex.quote(stage)],capture_output=True,timeout=30)
   r['remote_stage_removed']=cleanup.returncode==0
  r['success']=bool(r['native_tests_passed'] and r['artifacts_exported'] and r['remote_stage_removed'])
  r['native_execution_pending']=not r['success'];save()
 print(json.dumps({k:r.get(k) for k in ('success','native_tests_passed','remote_stage_removed','error')}),flush=True)
 return 0 if r['success'] else 1
if __name__=='__main__':raise SystemExit(main())
