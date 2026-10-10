#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Local source-table, protocol and real-kernel-header checks; no GPU or SSH."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OWNED = ['include/hackynavi/boot_backend_stop.hpp','src/boot_backend_stop.cpp',
         'tests/boot_backend_stop_tests.cpp','docs/inherited-hdmi-terminal-shutdown.md',
         'scripts/check-boot-backend-stop-local.py']
DEPENDENCIES = ['include/hackynavi/boot_display_backend.hpp','src/boot_display_backend.cpp',
                'include/hackynavi/boot_display_stop.hpp','src/boot_display_stop.cpp',
                'include/hackynavi/boot_display.hpp','src/boot_display.cpp',
                'include/hackynavi/discovery.hpp','src/discovery.cpp',
                'include/hackynavi/memory_layout.hpp','src/memory_layout.cpp',
                'include/hackynavi/vram_allocator.hpp','src/vram_allocator.cpp',
                'include/hackynavi/rom.hpp','src/rom.cpp','include/hackynavi/dmcub.hpp','src/dmcub.cpp']
UPSTREAM = ['.research/linux/'+name for name in ['dcn_3_2_0_offset.h','dcn_3_2_0_sh_mask.h',
            'dcn10_mpc.c','dcn10_hwseq.c','dcn20_hwseq.c','dcn32_optc.c','dcn10_optc.c','dcn20_opp.c',
            'dcn32_init.c','dcn32_resource.c','dcn32_dccg.c','dcn32_hwseq.c','dcn10_link_encoder.c',
            'dcn32_dio_link_encoder.c','dcn32_dio_stream_encoder.c','dcn30_dio_stream_encoder.c',
            'dcn10_stream_encoder.c','link_dpms.c','link_ddc.c','link_hwss_dio.c']]
UPSTREAM += ['.research/inherited-hdmi/'+name for name in ['fetch.json','dce110_hwseq.c','dcn30_afmt.c','command_table2.c']]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def register_oracle():
    raw = (ROOT / '.research/linux/dcn_3_2_0_offset.h').read_text()
    offsets = {name: int(value, 0) for name, value in re.findall(r'^#define (reg\w+)\s+(0x[\da-f]+|\d+)\s*$', raw, re.M)}
    source = (ROOT / 'src/boot_backend_stop.cpp').read_text()
    groups = {
        'linkOffsets': (5, ['regDIO_LINK{}_CNTL', 'regDIG{}_DIG_BE_CNTL', 'regDIG{}_DIG_BE_EN_CNTL']),
        'frontendOffsets': (5, ['regDIG{}_DIG_FE_CNTL', 'regDIG{}_DIG_FIFO_CTRL0', 'regDP{}_DP_VID_STREAM_CNTL',
                               'regDIG{}_HDMI_CONTROL', 'regDIG{}_HDMI_VBI_PACKET_CONTROL', 'regDIG{}_HDMI_GC']),
        'timingOffsets': (4, ['regOTG{}_OTG_CONTROL', 'regOTG{}_OTG_CLOCK_CONTROL', 'regOTG{}_OTG_MASTER_UPDATE_LOCK',
                             'regOTG{}_OTG_GLOBAL_CONTROL2', 'regOTG{}_OTG_GSL_CONTROL', 'regOTG{}_OTG_STEREO_CONTROL',
                             'regOTG{}_OTG_3D_STRUCTURE_CONTROL', 'regOTG{}_OTG_INTERLACE_CONTROL',
                             'regODM{}_OPTC_INPUT_GLOBAL_CONTROL', 'regODM{}_OPTC_DATA_SOURCE_SELECT',
                             'regODM{}_OPTC_DATA_FORMAT_CONTROL', 'regODM{}_OPTC_MEMORY_CONFIG']),
        'combinerOffsets': (4, ['regMPCC{}_MPCC_TOP_SEL', 'regMPCC{}_MPCC_BOT_SEL', 'regMPCC{}_MPCC_OPP_ID',
                               'regMPCC{}_MPCC_UPDATE_LOCK_SEL', 'regMPCC{}_MPCC_STATUS', 'regMPC_OUT{}_MUX'])}
    result = []
    for group, (count, names) in groups.items():
        body = re.search(r'const uint16_t ' + group + r'\[\]=\{([^}]+)\}', source).group(1)
        actual = [int(x.strip(), 0) for x in body.split(',')]
        assert len(actual) == len(names)
        for index, name in enumerate(names):
            stride = (1 if index == 0 else 0x100) if group == 'linkOffsets' else 0x100 if group == 'frontendOffsets' else (0x80 if index < 8 else 0x10) if group == 'timingOffsets' else (4 if index == 5 else 0x15)
            base = 3 if group == 'combinerOffsets' else 2
            for instance in range(count):
                symbol = name.format(chr(ord('A') + instance) if group == 'linkOffsets' and index == 0 else instance)
                assert offsets[symbol] == actual[index] + stride * instance, symbol
                assert offsets[symbol + '_BASE_IDX'] == base, symbol
                result.append({'symbol': symbol, 'dword': offsets[symbol], 'base_index': base})
    assert len(result) == 117
    for base_name, start, stride, source_token in [('regVTG{}_CONTROL',0x52e,1,'0x52e +'),
            ('regDPG{}_DPG_CONTROL',0x1854,0x5a,'0x1854+'),('regDPG{}_DPG_STATUS',0x185b,0x5a,'0x185b+')]:
        assert source_token in source
        for n in range(4):
            name=base_name.format(n)
            assert offsets[name]==start+n*stride and offsets[name+'_BASE_IDX']==2
            result.append({'symbol':name,'dword':offsets[name],'base_index':2})
    masks=(ROOT/'.research/linux/dcn_3_2_0_sh_mask.h').read_text()
    fields={name:int(value,0) for name,value in re.findall(r'^#define (\w+_MASK)\s+(0x[0-9A-Fa-f]+)L?\s*$',masks,re.M)}
    expected={'VTG0_CONTROL__VTG0_ENABLE_MASK':0x80000000,'DPG0_DPG_CONTROL__DPG_EN_MASK':1,
              'DPG0_DPG_STATUS__DPG_DOUBLE_BUFFER_PENDING_MASK':1,'MPCC0_MPCC_STATUS__MPCC_IDLE_MASK':1,
              'MPCC0_MPCC_STATUS__MPCC_BUSY_MASK':2,'MPCC0_MPCC_STATUS__MPCC_DISABLED_MASK':4,
              'OTG0_OTG_CONTROL__OTG_CURRENT_MASTER_EN_STATE_MASK':0x10000,
              'OTG0_OTG_CLOCK_CONTROL__OTG_BUSY_MASK':0x10000,'DIG0_DIG_FE_CNTL__DIG_SYMCLK_FE_ON_MASK':0x1000000}
    for name,value in expected.items():assert fields[name]==value
    assert sum(v for k,v in fields.items() if k.startswith('DPG0_DPG_CONTROL__'))==0x01ff3171
    se=(ROOT/'.research/linux/dcn32_dio_stream_encoder.c').read_text().split('static const struct stream_encoder_funcs dcn32_str_enc_funcs = {')[1].split('};')[0]
    assert '.disable_fifo' not in se and '.enable_stream' not in se and '.set_input_mode = enc32_set_dig_input_mode' in se
    cc=(ROOT/'.research/linux/dcn32_dccg.c').read_text().split('static const struct dccg_funcs dccg32_funcs = {')[1].split('};')[0]
    assert '.disable_symclk_se' not in cc
    return {'registers':result,'field_masks':expected,'actual_dcn32_optional_callbacks_absent':True}


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
              'commands': [], 'validation_scope': 'Disconnected MPC/OTG numeric protocol tests using actual API-created stopped HUBP state; no target observation or firmware/sink proof'}

    def run(command):
        result = subprocess.run([str(x) for x in command], cwd=ROOT, capture_output=True, text=True, timeout=120)
        report['commands'].append({'args': [str(x) for x in command], 'exit_code': result.returncode,
                                   'stdout': result.stdout, 'stderr': result.stderr})
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    try:
        report['register_oracle'] = register_oracle()
        common = [zig, 'c++', '-std=c++11', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-Iinclude']
        inputs = ['src/boot_backend_stop.cpp','src/boot_display_backend.cpp','src/boot_display_stop.cpp','src/boot_display.cpp','src/discovery.cpp','src/rom.cpp','src/dmcub.cpp','src/memory_layout.cpp','src/vram_allocator.cpp','tests/boot_backend_stop_tests.cpp']
        for name, extra in [('strict', []), ('ubsan', ['-fsanitize=undefined', '-fno-sanitize-recover=all'])]:
            exe = out / (name + '.exe')
            run([*common, *extra, *inputs, '-o', exe])
            run([exe])
        kernel = [zig, 'c++', '-target', 'x86_64-macos.15.0', '-std=c++11', '-mkernel', '-fapple-kext',
                  '-DKERNEL', '-DKERNEL_EXTENSION', '-nostdinc', '-nostdlibinc', '-I', '.research/MacKernelSDK/Headers',
                  '-I', 'include', '-fno-exceptions', '-fno-rtti', '-fno-builtin', '-fno-common',
                  '-fno-stack-protector', '-mno-red-zone', '-mno-mmx', '-mno-sse', '-mno-sse2',
                  '-Wall', '-Wextra', '-Werror', '-Wframe-larger-than=2048', '-O2']
        run([*kernel, '-M', '-MT', 'backend.o', '-MF', out / 'kernel-before.d', 'src/boot_backend_stop.cpp'])
        def dependencies(path):
            text = path.read_text().replace('\\\n', ' ')
            paths = text.split(':', 1)[1].split()
            return {p: sha(ROOT / p) for p in paths}
        report['kernel_dependencies'] = dependencies(out / 'kernel-before.d')
        run([*kernel, '-MD', '-MT', 'backend.o', '-MF', out / 'kernel-compiled.d', '-c',
             'src/boot_backend_stop.cpp', '-o', out / 'backend-kernel.o'])
        assert report['kernel_dependencies'] == dependencies(out / 'kernel-compiled.d')
        report['artifacts'] = {p.name: sha(p) for p in out.iterdir() if p.suffix in ('.exe', '.o')}
        report['success'] = True
    finally:
        report['inputs_unchanged'] = all(sha(ROOT / p) == h for p, h in report['source_files'].items())
        report['success'] &= report['inputs_unchanged']
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    if not report['success']:
        raise RuntimeError('Focused validation failed')
    report['semantic_cases'] = ['same real stopped State plus fresh all-domain drain', 'OPP and TG live prerequisites', 'five MPC writes and active idle completion', 'four timing writes and exact final stopped check', 'preserved FIFO latch and dynamic FE clock-status', 'all nine write ambiguities', 'read failures at distinct registers and mutation boundaries', 'OFF-domain avoidance', 'stale power/cursor/DMDATA/DPG/clock rejection', 'terminal no-retry state']
    (out / 'report.json').write_text(json.dumps(report,indent=2)+'\n')
    freeze = {'source_files': report['source_files'], 'report_sha256': sha(out / 'report.json'),
              'kernel_dependencies': report['kernel_dependencies'], 'no_target_or_gpu_execution': True}
    (out / 'source-freeze.json').write_text(json.dumps(freeze, indent=2) + '\n')
    print(json.dumps({'freeze_sha256': sha(out / 'source-freeze.json'), 'report_sha256': sha(out / 'report.json')}))


if __name__ == '__main__':
    main()
