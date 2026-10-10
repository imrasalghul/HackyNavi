#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run an immutable offline graphics compiler snapshot on the bounded Mac lane."""
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
 assert r['prepared'] and r['success'] and not r['remote_executed'] and sha(raw)==r['archive_sha256']
 expected=r['source_files'];data={}
 with tarfile.open(fileobj=io.BytesIO(raw),mode='r:gz') as tar:
  members=tar.getmembers();assert len(members)==len(expected)+1
  assert {m.name for m in members}==set(expected)|{'manifest.json'}
  assert sum(m.size for m in members)<209*1024*1024
  stage_cap=(256 if r.get('native_relink') else 160)*1024*1024
  assert r['remote_storage_cap_bytes']==stage_cap
  assert sum(m.size for m in members)+len(raw)+256*1024<=stage_cap
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
  stage=subprocess.check_output(ssh+['test "$(uname -sm)" = "Darwin x86_64" && test "$(df -Pk "$HOME" | tail -1 | awk \'{print $4}\')" -ge 1048576 && mktemp -d "$HOME/Library/Caches/HackyNavi-GraphicsNative.XXXXXX"'],text=True,timeout=30).strip()
  if not re.fullmatch(r'/Users/[^/\r\n]+/Library/Caches/HackyNavi-GraphicsNative\.[A-Za-z0-9]+',stage):stage=None;raise RuntimeError('Invalid stage')
  r['remote_stage']=stage;save()
  bootstrap='STAGE='+repr(stage)+'\nFILES='+repr(data)+'\nHASH='+repr(sha(raw))+'\nCAP='+repr(stage_cap)+'\n'+'''
import hashlib,subprocess,sys,tarfile
from pathlib import Path
s=Path(STAGE).resolve();assert str(s)==STAGE
a=s/'inputs.tar.gz';assert hashlib.sha256(a.read_bytes()).hexdigest()==HASH
with tarfile.open(a,'r:gz') as t:
 ms=t.getmembers();assert len(ms)==len(FILES) and {m.name for m in ms}==set(FILES)
 assert sum(m.size for m in ms)+a.stat().st_size+Path(__file__).stat().st_size<=CAP
 for m in ms:
  assert m.isfile() and m.size==FILES[m.name]['bytes']
  v=t.extractfile(m).read();assert hashlib.sha256(v).hexdigest()==FILES[m.name]['sha256']
  p=(s/m.name).resolve();assert p.is_relative_to(s)
  p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(v);p.chmod(FILES[m.name]['mode'])
# The exact host archive is retained locally; after verified extraction the
# redundant remote tar can be removed before linking to keep the stage small.
a.unlink()
raise SystemExit(subprocess.call([sys.executable,str(s/'scripts/run_native.py'),str(s)]))
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
  assert set(n['artifacts'])=={'hackynavi-aco-ngg','hackynavi-aco-fragment','hackynavi-aco-fragment-buffer','hackynavi-aco-vertex-buffer','libHackyNaviGraphicsCompiler.dylib','libHackyNaviColorLayout.dylib','color-layout-tests'}
  for name,item in n['artifacts'].items():
   if name in expected: assert item==expected[name]
   else: assert n.get('native_relink') and name in ('libHackyNaviGraphicsCompiler.dylib','libHackyNaviColorLayout.dylib')
   assert 0<item['bytes']<64*1024*1024 and re.fullmatch('[a-f0-9]{64}',item['sha256'])
   # Verify the exact actual native output, including Apple-relinked dylibs.
   subprocess.run(scp+[a.host+':'+stage+'/'+name,str(out/name)],check=True,timeout=60)
   assert sha((out/name).read_bytes())==item['sha256']
  subprocess.run(scp+[a.host+':'+stage+'/results/compiler/report.json',str(out/'compiler-report.json')],check=True,timeout=60)
  subprocess.run(scp+[a.host+':'+stage+'/results/actual-layouts.bin',str(out/'actual-layouts.bin')],check=True,timeout=60)
  assert sha((out/'actual-layouts.bin').read_bytes())==expected['fixtures/actual-layouts.bin']['sha256']
  r['exports']={name:sha((out/name).read_bytes()) for name in list(n['artifacts'])+['compiler-report.json','actual-layouts.bin']}
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
