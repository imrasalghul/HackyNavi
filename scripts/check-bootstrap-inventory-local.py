#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Offline bootstrap inventory fault coverage, pinned offsets and real kernel headers."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
COMMIT='3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0'
SOURCES=['src/bootstrap_quiesce.cpp','src/ip_layout.cpp','src/discovery.cpp','src/rom.cpp']
TRACKED=SOURCES+['include/hackynavi/bootstrap_quiesce.hpp','tests/bootstrap_guard_inventory_tests.cpp',
                'tests/bootstrap_quiesce_tests.cpp','scripts/check-bootstrap-inventory-local.py']
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def oracle():
    metadata=json.loads((ROOT/'.research/linux/gc_11_0_0_offset.h.source.json').read_text())
    assert metadata['commit']==COMMIT
    manifest=json.loads((ROOT/'.research/linux/boot_display_sources.json').read_text())
    mp=next(x for x in manifest['files'] if x['local']=='mp_13_0_0_offset.h')
    names={'gc_11_0_0_offset.h':metadata['sha256'],'mp_13_0_0_offset.h':mp['sha256'],
           'mmhub_3_0_0_offset.h':'b0ddff6c2f9f36ce4708d5a6ad3e66c4b8ce565cb68a141323f4556ce6f5315c'}
    defs={}
    for name,digest in names.items():
        p=ROOT/'.research/linux'/name;assert sha(p)==digest
        defs[name]={a:int(b,0) for a,b in re.findall(r'^#define (reg\w+)\s+(0x[0-9a-fA-F]+|\d+)\s*$',p.read_text(),re.M)}
    gc,mp,mm=[defs[n] for n in names]
    catalogue=[]
    controls=[('CP_ME_CNTL',0x803,1),('CP_MEC_CNTL',0x802,1),('CP_MEC_RS64_CNTL',0x2904,1),
              ('CP_MEC_ISA_CNTL',0x2903,1),('CP_GFX_CNTL',0x2a00,1),('CP_MES_CNTL',0x2807,1),
              ('CP_STAT',0xf40,0),('GRBM_STATUS',0xda4,0)]
    def add(d,name,offset,base):
        symbol='reg'+name;assert d[symbol]==offset and d[symbol+'_BASE_IDX']==base
        catalogue.append({'symbol':symbol,'offset_dwords':offset,'base_index':base})
    add(mp,'MP0_SMN_C2PMSG_81',0x91,0)
    for name,offset,base in controls:add(gc,name,offset,base)
    for i in range(16):add(gc,f'GCVM_CONTEXT{i}_CNTL',0x1688+i,0)
    for i in range(16):add(mm,f'MMVM_CONTEXT{i}_CNTL',0x740+i,0)
    for engine in range(2):
        for queue in range(8):
            for field,extra in [('RB',0),('IB',0xa)]:
                add(gc,f'SDMA{engine}_QUEUE{queue}_{field}_CNTL',0x80+queue*0x58+extra+engine*0x600,0)
        add(gc,f'SDMA{engine}_F32_CNTL',0x589a+engine*0x20,1)
        add(gc,f'SDMA{engine}_STATUS_REG',0x25+engine*0x600,0)
    assert len(catalogue)==77
    return {'commit':COMMIT,'source_sha256':names,'catalogue':catalogue,
            'mmhub_fresh_upstream_byte_comparison':'Matched pinned raw GitHub header during implementation on 2026-10-11; checker is offline.'}
def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',required=True,type=Path);args=ap.parse_args()
    out=args.output.resolve()
    if out.exists():ap.error('Use a fresh output directory')
    out.mkdir(parents=True);zig=ROOT/'.tools/zig-x86_64-windows-0.17.0/zig.exe'
    report={'success':False,'gpu_executed':False,'ssh_used':False,'target_executed':False,
            'sources':{p:sha(ROOT/p) for p in TRACKED},'compiler_sha256':sha(zig),'commands':[]}
    def run(args,expected=0):
        r=subprocess.run([str(x) for x in args],cwd=ROOT,capture_output=True,text=True,timeout=120)
        report['commands'].append({'args':[str(x) for x in args],'exit_code':r.returncode,'stdout':r.stdout,'stderr':r.stderr})
        if (expected==0 and r.returncode!=0) or (expected!=0 and r.returncode==0):raise RuntimeError(r.stdout+r.stderr)
    try:
        report['oracle']=oracle()
        common=[zig,'c++','-std=c++11','-Wall','-Wextra','-Wpedantic','-Werror','-Iinclude']
        for name,flags in [('strict',[]),('ubsan',['-fsanitize=undefined','-fno-sanitize-recover=all'])]:
            for test in ['bootstrap_guard_inventory','bootstrap_quiesce']:
                exe=out/(test+'-'+name+'.exe')
                run([*common,*flags,*SOURCES,'tests/'+test+'_tests.cpp','-o',exe]);run([exe])
        kernel=[zig,'c++','-target','x86_64-macos.15.0','-std=c++11','-mkernel','-fapple-kext','-DKERNEL','-DKERNEL_EXTENSION',
                '-nostdinc','-nostdlibinc','-I.research/MacKernelSDK/Headers','-Iinclude','-fno-exceptions','-fno-rtti',
                '-fno-builtin','-fno-common','-fno-stack-protector','-mno-red-zone','-mno-mmx','-mno-sse','-mno-sse2',
                '-Wall','-Wextra','-Werror','-Wframe-larger-than=2048','-O2']
        dep=out/'bootstrap.d'
        run([*kernel,'-M','-MT','bootstrap.o','-MF',dep,SOURCES[0]])
        dependencies=dep.read_text().replace('\\\n',' ').split(':',1)[1].split()
        report['kernel_dependencies']={p:sha(ROOT/p) for p in dependencies}
        run([*kernel,'-c',SOURCES[0],'-o',out/'bootstrap.o'])
        original=(ROOT/SOURCES[0]).read_text()
        controls=[('lost-guard-mismatch','out.guardMismatch[slot] |= bit','out.guardMismatch[slot] |= 0'),
                  ('lost-post-read-permission','if (!io.valid(io.context)) return out.status = GuardInventoryStatus::AccessDenied;\n        if (value == UINT32_MAX)',
                   'if (false) return out.status = GuardInventoryStatus::AccessDenied;\n        if (value == UINT32_MAX)')]
        for name,needle,replacement in controls:
            assert original.count(needle)==1
            altered=out/(name+'.cpp');altered.write_text(original.replace(needle,replacement))
            exe=out/(name+'.exe');run([*common,altered,*SOURCES[1:],'tests/bootstrap_guard_inventory_tests.cpp','-o',exe]);run([exe],expected=1)
        report['success']=True
    finally:
        report['inputs_unchanged']=all(sha(ROOT/p)==h for p,h in report['sources'].items()) and all(sha(ROOT/p)==h for p,h in report.get('kernel_dependencies',{}).items())
        report['success'] &= report['inputs_unchanged']
        report['artifacts']={p.name:sha(p) for p in out.iterdir() if p.suffix in ('.exe','.o')}
        (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    if not report['success']:raise RuntimeError('Validation failed')
    print(json.dumps({'success':True,'report_sha256':sha(out/'report.json')}))
if __name__=='__main__':main()
