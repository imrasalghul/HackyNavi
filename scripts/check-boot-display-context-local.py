#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check pinned DCN context register tables and real kernel compilation, offline."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT=Path(__file__).resolve().parents[1]
COMMIT='3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0'
OWNED=['include/hackynavi/boot_display_context.hpp','src/boot_display_context.cpp',
       'tests/boot_display_context_tests.cpp','scripts/check-boot-display-context-local.py',
       'docs/boot-display-context-capture.md']
LIBRARY=['src/boot_display_context.cpp','src/boot_display.cpp','src/discovery.cpp','src/rom.cpp','src/memory_layout.cpp']
EXTRA={
 'dcn32_init.c':('hwss/dcn32/dcn32_init.c','f9a828c7da2f984855a7ff3dfaeb2771507d90d9f64f327ef5cbf752e24b3a52'),
 'dcn32_optc.c':('optc/dcn32/dcn32_optc.c','7108d3181cb2c8f72f24fc6cfab6b09360fb8122b50db8647dcf5c1729c2901f'),
 'dcn10_optc.c':('optc/dcn10/dcn10_optc.c','43ad064bbb450851ab69fa0b0916371b5508f166254b58a0aa3a115ae6fc0951'),
 'dcn10_hwseq.c':('hwss/dcn10/dcn10_hwseq.c','2a7919ba3194f636a5cdb34810bbd1ee151b5a9ac1122fa7997da183b0ecc968'),
 'dcn32_hwseq.c':('hwss/dcn32/dcn32_hwseq.c','2907433372340405f9c3f83daf57d96bd441d5671a6faa43b1d2cb4cee3d3539'),
 'dcn32_hubbub.c':('hubbub/dcn32/dcn32_hubbub.c','c05a7c625cd0932ffe9d3eba698fc58272aecb13ae98c82d256d9509d8351017'),
}


def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()


def oracle():
    sources={}
    manifest=json.loads((ROOT/'.research/linux/boot_display_sources.json').read_text())
    assert manifest['commit']==COMMIT
    wanted=['dcn_3_2_0_offset.h','dcn_3_2_0_sh_mask.h','dcn32_hubp.c','dcn30_hubp.c','dcn30_hubbub.c']
    for name in wanted:
        entry=next(x for x in manifest['files'] if x['local']==name)
        assert sha(ROOT/'.research/linux'/name)==entry['sha256']
        sources[name]={**entry,'url':'https://github.com/torvalds/linux/blob/'+COMMIT+'/'+entry['path']}
    for name,(path,digest) in EXTRA.items():
        assert sha(ROOT/'.research/linux'/name)==digest
        sources[name]={'sha256':digest,'url':'https://github.com/torvalds/linux/blob/'+COMMIT+'/drivers/gpu/drm/amd/display/dc/'+path}
    vmid=ROOT/'.research/linux/dcn20_vmid.c'
    vmid_source=json.loads(vmid.with_suffix('.c.source.json').read_text())
    assert sha(vmid)==vmid_source['sha256'] and '/'+COMMIT+'/' in vmid_source['url']
    sources[vmid.name]=vmid_source
    text=(ROOT/'.research/linux/dcn_3_2_0_offset.h').read_text()
    offsets={a:int(b,0) for a,b in re.findall(r'^#define (reg\w+)\s+(0x[\da-f]+|\d+)\s*$',text,re.M)}
    source=(ROOT/'src/boot_display_context.cpp').read_text()

    def array(name):
        return [int(x,0) for x in re.search(r'\b'+name+r'\[\w+\]=\{([^}]+)\}',source)[1].split(',')]

    pipe,strides,vm=array('pipeOffsets'),array('pipeStrides'),array('vmOffsets')
    names=['regVTG{}_CONTROL','regOTG{}_OTG_CONTROL','regOTG{}_OTG_CLOCK_CONTROL',
           'regHUBP{}_HUBP_CLK_CNTL','regHUBP{}_DCHUBP_MALL_CONFIG','regHUBP{}_DCHUBP_MALL_SUB_VP']
    table=[]
    for p in range(4):
        for r,name in enumerate(names):
            symbol=name.format(p);assert offsets[symbol]==pipe[r]+p*strides[r] and offsets[symbol+'_BASE_IDX']==2
            table.append({'index':p*6+r,'symbol':symbol,'base_index':2,'dword_offset':offsets[symbol],
                          'target_byte_offset':hex((0x34c0+offsets[symbol])*4)})
        for symbol,expected in [(f'regDOMAIN{p}_PG_STATUS',0x81+p*2),(f'regDOMAIN{p}_PG_CONFIG',0x80+p*2)]:
            assert offsets[symbol]==expected and offsets[symbol+'_BASE_IDX']==2
    names=['regDCN_VM_AGP_BASE','regDCN_VM_CONTEXT0_CNTL']+[
        'regDCN_VM_CONTEXT0_PAGE_TABLE_'+part+'_ADDR_'+half for part in ['BASE','START','END'] for half in ['HI32','LO32']]
    for r,symbol in enumerate(names):
        assert offsets[symbol]==vm[r] and offsets[symbol+'_BASE_IDX']==2
        table.append({'index':24+r,'symbol':symbol,'base_index':2,'dword_offset':offsets[symbol],
                      'target_byte_offset':hex((0x34c0+offsets[symbol])*4)})
    masks=(ROOT/'.research/linux/dcn_3_2_0_sh_mask.h').read_text()
    fields={'VTG0_CONTROL__VTG0_ENABLE_MASK':0x80000000,'OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK':1,
            'OTG0_OTG_CONTROL__OTG_CURRENT_MASTER_EN_STATE_MASK':0x10000,
            'OTG0_OTG_CLOCK_CONTROL__OTG_BUSY_MASK':0x10000,'HUBP0_HUBP_CLK_CNTL__HUBP_CLOCK_ENABLE_MASK':1,
            'HUBP0_DCHUBP_MALL_CONFIG__USE_MALL_SEL_MASK':3,'HUBP0_DCHUBP_MALL_SUB_VP__USE_MALL_AT_START_LINE_MASK':1,
            'DCN_VM_CONTEXT0_CNTL__VM_CONTEXT0_PAGE_TABLE_DEPTH_MASK':6,
            'DCN_VM_CONTEXT0_CNTL__VM_CONTEXT0_PAGE_TABLE_BLOCK_SIZE_MASK':0x78}
    for symbol,value in fields.items():
        assert int(re.search(r'^#define '+symbol+r'\s+(0x[\da-fA-F]+)L',masks,re.M)[1],16)==value
    return {'sources':sources,'registers':table,'fields':fields}


def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',required=True,type=Path)
    ap.add_argument('--discovery',required=True,type=Path);args=ap.parse_args();out=args.output.resolve()
    if out.exists():ap.error('Use a fresh output directory')
    out.mkdir(parents=True);zig=ROOT/'.tools/zig-x86_64-windows-0.17.0/zig.exe'
    report={'success':False,'gpu_executed':False,'remote_executed':False,'target_executed':False,
            'source_files':{p:sha(ROOT/p) for p in set(OWNED+LIBRARY)},'compiler_sha256':sha(zig),'commands':[],
            'discovery':{'path':str(args.discovery.resolve()),'sha256':sha(args.discovery)}}

    def run(command):
        response=subprocess.run([str(x) for x in command],cwd=ROOT,capture_output=True,text=True,timeout=120)
        report['commands'].append({'args':[str(x) for x in command],'exit_code':response.returncode,
                                   'stdout':response.stdout,'stderr':response.stderr})
        if response.returncode:raise RuntimeError(response.stdout+response.stderr)

    try:
        report['oracle']=oracle()
        common=[zig,'c++','-std=c++11','-Wall','-Wextra','-Wpedantic','-Werror','-Iinclude']
        for kind,flags in [('strict',[]),('ubsan',['-fsanitize=undefined','-fno-sanitize-recover=all'])]:
            exe=out/(kind+'.exe');run([*common,*flags,*LIBRARY,'tests/boot_display_context_tests.cpp','-o',exe]);run([exe,args.discovery.resolve()])
        kernel=[zig,'c++','-target','x86_64-macos.15.0','-std=c++11','-mkernel','-fapple-kext','-DKERNEL','-DKERNEL_EXTENSION',
                '-nostdinc','-nostdlibinc','-I.research/MacKernelSDK/Headers','-Iinclude','-fno-exceptions','-fno-rtti',
                '-fno-builtin','-fno-common','-fno-stack-protector','-mno-red-zone','-mno-mmx','-mno-sse','-mno-sse2',
                '-Wall','-Wextra','-Werror','-Wframe-larger-than=2048','-O2']
        dep=out/'context-before.d';run([*kernel,'-M','-MT','context.o','-MF',dep,'src/boot_display_context.cpp'])

        def dependencies(p):
            paths=p.read_text().replace('\\\n',' ').split(':',1)[1].split()
            return {s:sha(ROOT/s) for s in paths}

        report['kernel_dependencies']=dependencies(dep)
        run([*kernel,'-MD','-MT','context.o','-MF',out/'context-compiled.d','-c','src/boot_display_context.cpp','-o',out/'context.o'])
        assert dependencies(out/'context-compiled.d')==report['kernel_dependencies']
        report['artifacts']={p.name:sha(p) for p in out.iterdir() if p.suffix in ('.exe','.o')};report['success']=True
    finally:
        report['inputs_unchanged']=all(sha(ROOT/p)==h for p,h in report['source_files'].items()) and sha(args.discovery)==report['discovery']['sha256']
        report['inputs_unchanged'] &= all(sha(ROOT/p)==h for p,h in report.get('kernel_dependencies',{}).items())
        report['success'] &= report['inputs_unchanged'];(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    if not report['success']:raise RuntimeError('Focused validation failed')
    print(json.dumps({'success':True,'report_sha256':sha(out/'report.json')}))


if __name__=='__main__':main()
