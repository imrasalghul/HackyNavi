#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Focused local inherited HDMI protocol checks. No SSH or GPU operation."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
ROOT = Path(__file__).resolve().parents[1]
OWNED = ['include/hackynavi/boot_hdmi_stream.hpp','src/boot_hdmi_stream.cpp',
         'tests/boot_hdmi_stream_tests.cpp','docs/boot-hdmi-stream.md',
         'scripts/check-boot-hdmi-stream-local.py']
DEPENDENCIES = ['include/hackynavi/boot_display_backend.hpp','src/boot_display_backend.cpp',
                'include/hackynavi/discovery.hpp','src/discovery.cpp',
                'include/hackynavi/rom.hpp','src/rom.cpp','include/hackynavi/dmcub.hpp','src/dmcub.cpp']
UPSTREAM = ['.research/linux/'+n for n in ['dcn_3_2_0_offset.h','dcn_3_2_0_sh_mask.h',
    'dcn32_resource.c','dcn32_dio_stream_encoder.c','dcn30_dio_stream_encoder.c',
    'dcn10_stream_encoder.c','dcn32_optc.c','dcn10_optc.c','dcn30_hwseq.c',
    'dcn32_init.c','dcn30_vpg.c','link_dpms.c','link_hwss_dio.c']]
UPSTREAM += ['.research/inherited-hdmi/'+n for n in ['fetch.json','dcn30_afmt.c',
    'dce110_hwseq.c','hdcp-fetch.json','hdcp_psp.c','hdcp.h','amdgpu_dm_hdcp.c']]

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def register_oracle():
    raw=(ROOT/'.research/linux/dcn_3_2_0_offset.h').read_text()
    definitions={n:int(v,0) for n,v in re.findall(r'^#define (reg\w+)\s+(0x[\da-f]+|\d+)\s*$',raw,re.M)}
    source=(ROOT/'src/boot_hdmi_stream.cpp').read_text()
    values=[int(x.strip(),0) for x in re.search(r'const uint16_t offsets\[\]=\{([^}]+)\}',source).group(1).split(',')]
    extra=['regDIG{}_AFMT_CNTL','regDIG{}_HDMI_METADATA_PACKET_CONTROL','regDIG{}_HDMI_AUDIO_PACKET_CONTROL',
        'regDIG{}_HDMI_INFOFRAME_CONTROL0','regDIG{}_HDMI_INFOFRAME_CONTROL1','regDIG{}_HDMI_ACR_PACKET_CONTROL',
        'regDIG{}_HDMI_GENERIC_PACKET_CONTROL0','regDIG{}_HDMI_GENERIC_PACKET_CONTROL6','regDIG{}_HDMI_GENERIC_PACKET_CONTROL5']
    extra += ['regDIG{}_HDMI_GENERIC_PACKET_CONTROL'+str(n) for n in [1,2,3,4,7,8,9,10]]
    extra += ['regDIG{}_HDMI_DB_CONTROL','regAFMT{}_AFMT_AUDIO_PACKET_CONTROL','regAFMT{}_AFMT_AUDIO_PACKET_CONTROL2',
        'regAFMT{}_AFMT_INFOFRAME_CONTROL0','regAFMT{}_AFMT_AUDIO_SRC_CONTROL','regAFMT{}_AFMT_VBI_PACKET_CONTROL',
        'regAFMT{}_AFMT_MEM_PWR','regVPG{}_VPG_GSP_FRAME_UPDATE_CTRL','regVPG{}_VPG_GSP_IMMEDIATE_UPDATE_CTRL',
        'regVPG{}_VPG_GENERIC_STATUS','regVPG{}_VPG_MEM_PWR','regAFMT{}_AFMT_STATUS','regDIG{}_HDMI_STATUS']
    assert len(extra)==len(values)==30
    result=[]
    for n in range(5):
        for template,value in zip(extra,values):
            name=template.format(n)
            assert definitions[name]==value+n*0x100 and definitions[name+'_BASE_IDX']==2,name
            result.append({'name':name,'dword':definitions[name],'base':2})
    groups={
      'linkOffsets':(5,['regDIO_LINK{}_CNTL','regDIG{}_DIG_BE_CNTL','regDIG{}_DIG_BE_EN_CNTL']),
      'frontendOffsets':(5,['regDIG{}_DIG_FE_CNTL','regDIG{}_DIG_FIFO_CTRL0','regDP{}_DP_VID_STREAM_CNTL','regDIG{}_HDMI_CONTROL','regDIG{}_HDMI_VBI_PACKET_CONTROL','regDIG{}_HDMI_GC']),
      'timingOffsets':(4,['regOTG{}_OTG_CONTROL','regOTG{}_OTG_CLOCK_CONTROL','regOTG{}_OTG_MASTER_UPDATE_LOCK','regOTG{}_OTG_GLOBAL_CONTROL2','regOTG{}_OTG_GSL_CONTROL','regOTG{}_OTG_STEREO_CONTROL','regOTG{}_OTG_3D_STRUCTURE_CONTROL','regOTG{}_OTG_INTERLACE_CONTROL','regODM{}_OPTC_INPUT_GLOBAL_CONTROL','regODM{}_OPTC_DATA_SOURCE_SELECT','regODM{}_OPTC_DATA_FORMAT_CONTROL','regODM{}_OPTC_MEMORY_CONFIG']),
      'combinerOffsets':(4,['regMPCC{}_MPCC_TOP_SEL','regMPCC{}_MPCC_BOT_SEL','regMPCC{}_MPCC_OPP_ID','regMPCC{}_MPCC_UPDATE_LOCK_SEL','regMPCC{}_MPCC_STATUS','regMPC_OUT{}_MUX'])}
    for group,(count,names) in groups.items():
        values=[int(x.strip(),0) for x in re.search('const uint16_t '+group+r'\[\]=\{([^}]+)\}',source).group(1).split(',')]
        assert len(names)==len(values)
        for q,(template,value) in enumerate(zip(names,values)):
            stride=(1 if q==0 else 0x100) if group=='linkOffsets' else 0x100 if group=='frontendOffsets' else (0x80 if q<8 else 0x10) if group=='timingOffsets' else (4 if q==5 else 0x15)
            base=3 if group=='combinerOffsets' else 2
            for n in range(count):
                name=template.format(chr(65+n) if group=='linkOffsets' and q==0 else n)
                assert definitions[name]==value+n*stride and definitions[name+'_BASE_IDX']==base,name
                result.append({'name':name,'dword':definitions[name],'base':base})
    assert 'd.pipe+0x1b49+route_.timingIndex*0x80u' in source
    for n in range(4):
        name=f'regOTG{n}_OTG_STATUS'
        assert definitions[name]==0x1b49+n*0x80 and definitions[name+'_BASE_IDX']==2
        result.append({'name':name,'dword':definitions[name],'base':2})
    masks=(ROOT/'.research/linux/dcn_3_2_0_sh_mask.h').read_text()
    fields={n:int(v,0) for n,v in re.findall(r'^#define (\w+_MASK)\s+(0x[0-9A-Fa-f]+)L?\s*$',masks,re.M)}
    expected={'OTG0_OTG_STATUS__OTG_V_BLANK_MASK':1,'OTG0_OTG_STATUS__OTG_V_ACTIVE_DISP_MASK':2,
      'DIG0_HDMI_GC__HDMI_GC_AVMUTE_MASK':1,'DIG0_HDMI_STATUS__HDMI_ACTIVE_AVMUTE_MASK':1,
      'AFMT0_AFMT_AUDIO_PACKET_CONTROL__AFMT_AUDIO_SAMPLE_SEND_MASK':1,
      'AFMT0_AFMT_AUDIO_PACKET_CONTROL__AFMT_AUDIO_SAMPLE_SEND_DOUBLE_BUFFER_ENABLE_MASK':0x10,
      'AFMT0_AFMT_AUDIO_PACKET_CONTROL__AFMT_RESET_FIFO_WHEN_AUDIO_DIS_MASK':0x800,
      'DIG0_HDMI_DB_CONTROL__HDMI_DB_DISABLE_MASK':0x1000,
      'DIG0_AFMT_CNTL__AFMT_AUDIO_CLOCK_EN_MASK':1,'DIG0_AFMT_CNTL__AFMT_AUDIO_CLOCK_ON_MASK':0x100,
      'AFMT0_AFMT_INFOFRAME_CONTROL0__AFMT_AUDIO_INFO_UPDATE_MASK':0x80,
      'AFMT0_AFMT_STATUS__AFMT_AZ_HBR_ENABLE_MASK':0x100,
      'AFMT0_AFMT_AUDIO_PACKET_CONTROL__AFMT_AUDIO_FIFO_OVERFLOW_ACK_MASK':0x800000,
      'AFMT0_AFMT_AUDIO_PACKET_CONTROL__AFMT_60958_CS_UPDATE_MASK':0x4000000,
      'AFMT0_AFMT_AUDIO_PACKET_CONTROL__AFMT_AZ_AUDIO_ENABLE_CHG_ACK_MASK':0x40000000}
    for n,v in expected.items():assert fields[n]==v,n
    assert sum(v for n,v in fields.items() if n.startswith('OTG0_OTG_STATUS__') and not n.endswith('3D_STRUCTURE_MASK'))==0x7000f
    for prefix,want in [('DIG0_HDMI_ACR_PACKET_CONTROL__',0x80071133),('DIG0_HDMI_GENERIC_PACKET_CONTROL10__',0x7fffffff),('AFMT0_AFMT_VBI_PACKET_CONTROL__',0x011f0000)]:
        assert sum(v for n,v in fields.items() if n.startswith(prefix))==want,prefix
    hwseq=(ROOT/'.research/linux/dcn30_hwseq.c').read_text().split('void dcn30_set_avmute(')[1].split('void dcn30_update_info_frame')[0]
    assert 'i < 3' in hwseq and hwseq.count('CRTC_STATE_VACTIVE')==2 and hwseq.count('CRTC_STATE_VBLANK')==1
    assert '.wait_for_state = optc1_wait_for_state' in (ROOT/'.research/linux/dcn32_optc.c').read_text()
    afmt=(ROOT/'.research/inherited-hdmi/dcn30_afmt.c').read_text().split('static struct afmt_funcs dcn30_afmt_funcs = {')[1].split('};')[0]
    assert '.afmt_powerdown' not in afmt and '.afmt_poweron' not in afmt and '.audio_mute_control' in afmt
    return {'registers':result,'field_masks':expected,'actual_frame_sequence_and_function_tables':True}

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    out = args.output.resolve()
    if out.exists():
        ap.error('Use a fresh evidence directory')
    out.mkdir(parents=True)
    zig = ROOT / '.tools/zig-x86_64-windows-0.17.0/zig.exe'
    bound = OWNED + DEPENDENCIES + UPSTREAM
    report = {'success': False, 'gpu_executed': False, 'remote_executed': False,
              'source_files': {p: sha(ROOT / p) for p in bound}, 'compiler_sha256': sha(zig),
              'commands': [], 'validation_scope': 'Disconnected inherited HDMI AVMUTE/audio numeric protocol tests; no HDCP/sink/native ownership or target execution'}

    def run(command):
        result = subprocess.run([str(x) for x in command], cwd=ROOT, capture_output=True, text=True, timeout=120)
        report['commands'].append({'args': [str(x) for x in command], 'exit_code': result.returncode,
                                   'stdout': result.stdout, 'stderr': result.stderr})
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    try:
        report['register_oracle'] = register_oracle()
        common = [zig, 'c++', '-std=c++11', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-Iinclude']
        inputs = ['src/boot_hdmi_stream.cpp','src/boot_display_backend.cpp','src/discovery.cpp','src/rom.cpp','src/dmcub.cpp','tests/boot_hdmi_stream_tests.cpp']
        for name, extra in [('strict', []), ('ubsan', ['-fsanitize=undefined', '-fno-sanitize-recover=all'])]:
            exe = out / (name + '.exe')
            run([*common, *extra, *inputs, '-o', exe])
            run([exe])
        kernel = [zig, 'c++', '-target', 'x86_64-macos.15.0', '-std=c++11', '-mkernel', '-fapple-kext',
                  '-DKERNEL', '-DKERNEL_EXTENSION', '-nostdinc', '-nostdlibinc', '-I', '.research/MacKernelSDK/Headers',
                  '-I', 'include', '-fno-exceptions', '-fno-rtti', '-fno-builtin', '-fno-common',
                  '-fno-stack-protector', '-mno-red-zone', '-mno-mmx', '-mno-sse', '-mno-sse2',
                  '-Wall', '-Wextra', '-Werror', '-Wframe-larger-than=2048', '-O2']
        run([*kernel, '-M', '-MT', 'backend.o', '-MF', out / 'kernel-before.d', 'src/boot_hdmi_stream.cpp'])
        def dependencies(path):
            text = path.read_text().replace('\\\n', ' ')
            paths = text.split(':', 1)[1].split()
            return {p: sha(ROOT / p) for p in paths}
        report['kernel_dependencies'] = dependencies(out / 'kernel-before.d')
        run([*kernel, '-MD', '-MT', 'backend.o', '-MF', out / 'kernel-compiled.d', '-c',
             'src/boot_hdmi_stream.cpp', '-o', out / 'stream-kernel.o'])
        assert report['kernel_dependencies'] == dependencies(out / 'kernel-compiled.d')
        report['artifacts'] = {p.name: sha(p) for p in out.iterdir() if p.suffix in ('.exe', '.o')}
        report['success'] = True
    finally:
        report['inputs_unchanged'] = all(sha(ROOT / p) == h for p, h in report['source_files'].items())
        report['success'] &= report['inputs_unchanged']
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    if not report['success']:
        raise RuntimeError('Focused validation failed')
    report['semantic_cases'] = ['independent FE/TG/AFMT identity', 'full selected packet/audio inventory retained', 'three distinct actual-status frame cycles', 'stalled and contradictory status rejection', 'active AVMUTE required', 'two ambiguous writes and permission loss', 'final audio and clock reassertion', 'unknown modes and stale raw baseline', 'one shared frame budget and terminal no retry']
    (out / 'report.json').write_text(json.dumps(report,indent=2)+'\n')
    freeze = {'source_files': report['source_files'], 'report_sha256': sha(out / 'report.json'),
              'kernel_dependencies': report['kernel_dependencies'], 'no_target_or_gpu_execution': True}
    (out / 'source-freeze.json').write_text(json.dumps(freeze, indent=2) + '\n')
    print(json.dumps({'freeze_sha256': sha(out / 'source-freeze.json'), 'report_sha256': sha(out / 'report.json')}))


if __name__ == '__main__':
    main()
