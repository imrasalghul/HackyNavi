#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Actual TA metadata and offline PSP-ring checks; no SSH or GPU execution."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
ROOT = Path(__file__).resolve().parents[1]
OWNED = ['include/hackynavi/psp_ta.hpp','src/psp_ta.cpp','include/hackynavi/psp_ring.hpp',
         'src/psp_ring.cpp','tests/psp_ta_tests.cpp','tests/psp_ta_abi_oracle.cpp',
         'docs/psp-trusted-applications.md','scripts/check-psp-ta-local.py',
         'kext/HackyNavi/HackyNaviFirmwareTA.cpp','kext/HackyNavi/HackyNaviFirmwareController.hpp',
         'kext/HackyNavi/HackyNaviFirmwareController.cpp','include/hackynavi/firmware_resources.hpp',
         'firmware/manifest.json','scripts/embed-firmware.py','scripts/build-kext.sh',
         'scripts/cross-check-kext.py','CMakeLists.txt','scripts/build-host.py']
SOURCES = ['src/rom.cpp','src/firmware.cpp','src/discovery.cpp','src/ip_layout.cpp',
           'src/memory_layout.cpp','src/vram_allocator.cpp','src/vram_transfer.cpp','src/psp.cpp','src/psp_ring.cpp']
FIRMWARE = '.research/firmware/amdgpu/psp_13_0_0_ta.bin'
UPSTREAM = ['.research/linux/'+p for p in ['amdgpu_ucode.h','amdgpu_ucode.c','amdgpu_psp.h',
             'amdgpu_psp.c','psp_gfx_if.h','psp_v13_0.c']]
UPSTREAM += ['.research/inherited-hdmi/'+p for p in ['hdcp-fetch.json','hdcp_psp.c','hdcp.h','amdgpu_dm_hdcp.c']]
UPSTREAM += [FIRMWARE,'.research/firmware/manifest.json','.research/firmware/LICENSES/LICENSE.amdgpu']

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()

def metadata():
    raw = (ROOT/FIRMWARE).read_bytes()
    assert sha(ROOT/FIRMWARE) == 'ac2454a36618d8568e798c6000bf3e19fdabf46d46038b92635adc3cb01bd198'
    manifest = json.loads((ROOT/'.research/firmware/manifest.json').read_text())
    assert manifest['commit'] == 'afabaf773c4c2e2c841429933a6a084a5af4d14d'
    count, = struct.unpack_from('<I',raw,32)
    records = []
    for i in range(count):
        kind, version, offset, size = struct.unpack_from('<4I',raw,36+16*i)
        records.append({'type':kind,'version':version,'offset':offset+256,'size':size,
                        'image_sha256':hashlib.sha256(raw[offset+256:offset+256+size]).hexdigest()})
    assert [r['type'] for r in records] == [1,4,5,3]
    ucode=(ROOT/'.research/linux/amdgpu_ucode.h').read_text()
    enum=ucode.split('enum ta_fw_type {')[1].split('};')[0]
    names=re.findall(r'\bTA_FW_TYPE_\w+',enum)
    assert names[4:6] == ['TA_FW_TYPE_PSP_HDCP','TA_FW_TYPE_PSP_DTM']
    header=(ROOT/'.research/linux/amdgpu_psp.h').read_text()
    for name in ['PSP_HDCP_SHARED_MEM_SIZE','PSP_DTM_SHARED_MEM_SIZE']:
        assert re.search(name+r'\s*=\s*0x4000',header)
    implementation=(ROOT/'.research/linux/amdgpu_psp.c').read_text()
    assert 'context->session_id = cmd->resp.session_id;' in implementation
    assert 'cmd->cmd.cmd_unload_ta.session_id = session_id;' in implementation
    assert 'cmd->cmd.cmd_invoke_cmd.session_id\t= session_id;' in implementation or \
           re.search(r'cmd->cmd.cmd_invoke_cmd.session_id\s*=\s*session_id;',implementation)
    return {'container_bytes':len(raw),'records':records,'stored_crc_interpretation':'metadata only; no invented polynomial or authentication',
            'upstream_enum_and_shared_sizes_verified':True}

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output',type=Path,required=True)
    args=ap.parse_args(); out=args.output.resolve()
    if out.exists(): ap.error('Use a fresh evidence directory')
    out.mkdir(parents=True)
    zig=ROOT/'.tools/zig-x86_64-windows-0.17.0/zig.exe'
    bound=list(dict.fromkeys(OWNED+SOURCES+UPSTREAM+['tests/psp_ring_fixture.hpp','tests/psp_ring_tests.cpp']))
    report={'success':False,'target_executed':False,'remote_executed':False,'gpu_executed':False,
            'source_files':{p:sha(ROOT/p) for p in bound},'compiler_sha256':sha(zig),'commands':[],
            'scope':'Actual-image metadata, source ABI oracle, offline fault/lifetime tests and real kernel-header compilation only',
            'kernel_dependencies':{}}
    def run(command):
        result=subprocess.run([str(x) for x in command],cwd=ROOT,capture_output=True,text=True,timeout=120)
        report['commands'].append({'args':[str(x) for x in command],'exit_code':result.returncode,
                                  'stdout':result.stdout,'stderr':result.stderr})
        if result.returncode: raise RuntimeError(result.stdout+result.stderr)
    def deps(path):
        paths=path.read_text().replace('\\\n',' ').split(':',1)[1].split()
        return {p:sha(ROOT/p) for p in paths}
    try:
        report['actual_firmware']=metadata()
        common=[zig,'c++','-std=c++11','-O2','-Wall','-Wextra','-Wpedantic','-Werror','-Iinclude']
        oracle=out/'abi-oracle.exe'
        # The unchanged Linux ABI uses an anonymous type inside an anonymous
        # union; allow that source extension only for this independent oracle.
        run([*common,'-Wno-nested-anon-types','-I.research/linux','tests/psp_ta_abi_oracle.cpp','-o',oracle]);run([oracle])
        for name,extra in [('strict',[]),('ubsan',['-fsanitize=undefined','-fno-sanitize-recover=all'])]:
            exe=out/(name+'.exe')
            run([*common,*extra,*SOURCES,'src/psp_ta.cpp','tests/psp_ta_tests.cpp','-o',exe]);run([exe,FIRMWARE])
        old=out/'existing-ring.exe'
        run([*common,*SOURCES,'tests/psp_ring_tests.cpp','-o',old]);run([old])
        kernel=[zig,'c++','-target','x86_64-macos.15.0','-std=c++14','-mkernel','-fapple-kext',
                '-DKERNEL','-DKERNEL_EXTENSION','-nostdinc','-nostdlibinc','-I','.research/MacKernelSDK/Headers',
                '-I','include','-fno-exceptions','-fno-rtti','-fno-builtin','-fno-common','-fno-stack-protector',
                '-mno-red-zone','-mno-mmx','-mno-sse','-mno-sse2','-Wall','-Wextra','-Werror',
                '-Wno-unused-parameter','-Wframe-larger-than=2048','-O2','-fstack-usage']
        for source in ['src/psp_ta.cpp','src/psp_ring.cpp',
                       'kext/HackyNavi/HackyNaviFirmwareTA.cpp','kext/HackyNavi/HackyNaviFirmwareController.cpp']:
            name=Path(source).stem; before=out/(name+'-before.d'); after=out/(name+'-compiled.d')
            run([*kernel,'-M','-MT','kernel.o','-MF',before,source])
            report['kernel_dependencies'][source]=deps(before)
            # Zig otherwise leaves Clang's .su alongside its private cache
            # object. Pin the cc1 output path so the retained evidence is real.
            run([*kernel,'-Xclang','-stack-usage-file','-Xclang',out/(name+'-kernel.su'),
                 '-MD','-MT','kernel.o','-MF',after,'-c',source,'-o',out/(name+'-kernel.o')])
            assert report['kernel_dependencies'][source] == deps(after)
        report['kernel_stack_usage']={}
        for stack in out.glob('*.su'):
            frames=[int(line.split('\t')[-2]) for line in stack.read_text().splitlines() if line]
            assert frames and max(frames)<=2048
            report['kernel_stack_usage'][stack.name]={'maximum_bytes':max(frames),'sha256':sha(stack)}
        assert len(report['kernel_stack_usage'])==4
        report['artifacts']={p.name:sha(p) for p in out.iterdir() if p.suffix in ('.exe','.o','.su')}
        report['semantic_cases']=['real image range/partition/type/alias admission','owned returned zero/nonzero sessions',
            'two independent TA slots and unload identity','request/reserved field corruption',
            'control/shared/staging aliases and released bindings','publication and response ambiguity retains ownership',
            'late observation never reopens a sealed ring','no TMR/ring destruction while TA retained',
            'existing non-TA PSP regression','no inherited authentication/session authority']
        report['success']=True
    finally:
        report['inputs_unchanged']=all(sha(ROOT/p)==h for p,h in report['source_files'].items())
        report['success'] &= report['inputs_unchanged']
        (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    if not report['success']: raise RuntimeError('Focused validation failed')
    freeze={'source_files':report['source_files'],'report_sha256':sha(out/'report.json'),
            'kernel_dependencies':report['kernel_dependencies'],'no_target_or_gpu_execution':True}
    (out/'source-freeze.json').write_text(json.dumps(freeze,indent=2)+'\n')
    print(json.dumps({'freeze_sha256':sha(out/'source-freeze.json'),'report_sha256':sha(out/'report.json')}))
if __name__=='__main__': main()
