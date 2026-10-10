#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Pinned DCN3.2 DP stream oracle, offline faults, and real kernel headers."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
COMMIT = '3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0'
OWNED = ['include/hackynavi/dp_stream.hpp', 'src/dp_stream.cpp',
         'tests/dp_stream_tests.cpp', 'scripts/check-dp-stream-local.py',
         'docs/dp-stream-programming.md']
LIBRARY = ['src/dp_stream.cpp', 'src/discovery.cpp', 'src/rom.cpp']
UPSTREAM = {
    'dcn10_stream_encoder.c': 'd28376338093ef2913ea3feef1c5e5d7d17f37f014077dbd4be00648108aa949',
    'dcn20_stream_encoder.c': '6b0ad29ddeea3617f31502ef98c2cfb752d529c2f833334a638c91b3411947a5',
    'dcn32_dio_stream_encoder.c': 'bdd78e32b5317b372bc89ac3b90d0ab19a70b03b4416bfa3bb9e45d87bced3f1',
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def oracle():
    manifest = json.loads((ROOT/'.research/dp-stream/source-manifest.json').read_text())
    assert manifest['commit'] == COMMIT
    sources = {}
    for name, digest in UPSTREAM.items():
        record = next(x for x in manifest['files'] if x['local'] == name)
        assert record['sha256'] == digest and '/'+COMMIT+'/' in record['url']
        assert sha(ROOT/'.research/dp-stream'/name) == digest
        sources[name] = record
    generated = json.loads((ROOT/'.research/linux/boot_display_sources.json').read_text())
    assert generated['commit'] == COMMIT
    for name in ['dcn_3_2_0_offset.h', 'dcn_3_2_0_sh_mask.h']:
        record = next(x for x in generated['files'] if x['local'] == name)
        assert sha(ROOT/'.research/linux'/name) == record['sha256']
        sources[name] = record
    offsets = {a: int(b, 0) for a, b in re.findall(r'^#define (reg\w+)\s+(0x[\da-f]+|\d+)\s*$',
               (ROOT/'.research/linux/dcn_3_2_0_offset.h').read_text(), re.M)}
    masks = {a: int(b, 16) for a, b in re.findall(r'^#define (\w+_MASK)\s+(0x[\da-fA-F]+)L',
             (ROOT/'.research/linux/dcn_3_2_0_sh_mask.h').read_text(), re.M)}
    source = (ROOT/'src/dp_stream.cpp').read_text()

    def array(name):
        return [int(x.strip(), 0) for x in re.search(r'\b'+name+r'\[\]=\{([^}]+)\}', source)[1].split(',')]

    names = ['DIG{}_DIG_FE_CNTL', 'DIG{}_DIG_FIFO_CTRL0', 'DIG{}_DIG_BE_CNTL',
             'DIG{}_DIG_BE_EN_CNTL', 'DIO_LINK{}_CNTL']
    names += ['DP{}_DP_'+n for n in ['CONFIG', 'LINK_CNTL', 'DPHY_CNTL', 'LINK_FRAMING_CNTL',
              'DPHY_INTERNAL_CTRL', 'DPHY_PRBS_CNTL', 'PIXEL_FORMAT', 'MSA_COLORIMETRY', 'MSA_MISC',
              'MSA_TIMING_PARAM1', 'MSA_TIMING_PARAM2', 'MSA_TIMING_PARAM3', 'MSA_TIMING_PARAM4',
              'VID_TIMING', 'VID_N', 'VID_M', 'VID_STREAM_CNTL', 'STEER_FIFO', 'SEC_CNTL',
              'SEC_CNTL2', 'SEC_FRAMING4', 'SEC_METADATA_TRANSMISSION', 'GSP11_CNTL', 'DSC_CNTL',
              'DB_CNTL', 'MSA_VBID_MISC', 'SEC_CNTL1', 'GSP8_CNTL', 'GSP9_CNTL', 'GSP10_CNTL', 'DPHY_SCRAM_CNTL']]
    table = []
    own = array('offsets')
    assert len(own) == len(names) == 36
    for n in range(5):
        for i, name in enumerate(names):
            symbol = 'reg'+name.format(chr(ord('A')+n) if i == 4 else n)
            assert offsets[symbol] == own[i]+n*(1 if i == 4 else 0x100)
            assert offsets[symbol+'_BASE_IDX'] == 2
            table.append({'symbol': symbol, 'base_index': 2, 'dword_offset': offsets[symbol]})
    names = ['CONTROL', 'CLOCK_CONTROL', 'STATUS_FRAME_COUNT', 'H_TOTAL', 'V_TOTAL', 'H_BLANK_START_END',
             'H_SYNC_A', 'H_SYNC_A_CNTL', 'V_BLANK_START_END', 'V_SYNC_A', 'V_SYNC_A_CNTL', 'INTERLACE_CONTROL']
    own = array('timingOffsets')
    assert len(own) == len(names) == 12
    for n in range(4):
        for i, name in enumerate(names):
            symbol = f'regOTG{n}_OTG_'+name
            assert offsets[symbol] == own[i]+n*0x80 and offsets[symbol+'_BASE_IDX'] == 2
            table.append({'symbol': symbol, 'base_index': 2, 'dword_offset': offsets[symbol]})
    fields = {}

    def group(prefix, names, expected):
        value = 0
        for name in names.split():
            symbol = prefix+'__'+name+'_MASK'
            fields[symbol] = masks[symbol]
            value |= masks[symbol]
        assert value == expected, (prefix, hex(value), hex(expected))

    group('DIG0_DIG_FE_CNTL', 'DIG_SOURCE_SELECT DIG_STEREOSYNC_SELECT DIG_STEREOSYNC_GATE_EN', 0x177)
    group('DIG0_DIG_FE_CNTL', 'DIG_DIGITAL_BYPASS_SELECT DIG_SPLIT_LINK_PIXEL_GROUPING DIG_INPUT_PIXEL_SELECT '
          'DOLBY_VISION_EN DIG_DIGITAL_BYPASS_EN TMDS_PIXEL_ENCODING TMDS_COLOR_FORMAT', 0xd017f000)
    group('DIG0_DIG_FE_CNTL', 'DIG_SYMCLK_FE_ON', 0x1000000)
    group('DIG0_DIG_FIFO_CTRL0', 'DIG_FIFO_ENABLE DIG_FIFO_RESET DIG_FIFO_READ_CLOCK_SRC '
          'DIG_FIFO_OUTPUT_PIXEL_MODE DIG_FIFO_RESET_DONE DIG_FIFO_ERROR', 0x30100183)
    group('DIG0_DIG_FIFO_CTRL0', 'DIG_FIFO_READ_START_LEVEL', 0x7c)
    group('DIG0_DIG_BE_CNTL', 'DIG_DUAL_LINK_ENABLE DIG_SWAP DIG_RB_SWITCH_EN DIG_FE_SOURCE_SELECT '
          'DIG_MODE DIG_HPD_SELECT', 0x70077f07)
    group('DIG0_DIG_BE_EN_CNTL', 'DIG_ENABLE', 1)
    group('DIO_LINKA_CNTL', 'ENC_TYPE_SEL', 3)
    group('DP0_DP_CONFIG', 'DP_UDI_LANES', 3)
    group('DP0_DP_LINK_CNTL', 'DP_LINK_TRAINING_COMPLETE DP_EMBEDDED_PANEL_MODE', 0x20010)
    group('DP0_DP_DPHY_CNTL', 'DPHY_ATEST_SEL_LANE0 DPHY_ATEST_SEL_LANE1 DPHY_ATEST_SEL_LANE2 '
          'DPHY_ATEST_SEL_LANE3 DPHY_FEC_EN DPHY_FEC_READY_SHADOW DPHY_FEC_ACTIVE_STATUS DPHY_BYPASS DPHY_SKEW_BYPASS', 0x101007f)
    group('DP0_DP_LINK_FRAMING_CNTL', 'DP_IDLE_BS_INTERVAL DP_VBID_DISABLE DP_VID_ENHANCED_FRAME_MODE', 0x1103ffff)
    group('DP0_DP_DPHY_PRBS_CNTL', 'DPHY_PRBS_EN', 1)
    group('DP0_DP_DPHY_SCRAM_CNTL', 'DPHY_SCRAMBLER_DIS DPHY_SCRAMBLER_ADVANCE DPHY_SCRAMBLER_BS_COUNT DPHY_SCRAMBLER_KCODE', 0x103ff11)
    group('DP0_DP_PIXEL_FORMAT', 'DP_PIXEL_ENCODING DP_COMPONENT_DEPTH DP_PIXEL_PER_CYCLE_PROCESSING_MODE', 0x47000007)
    group('DP0_DP_MSA_COLORIMETRY', 'DP_MSA_MISC0', 0xff000000)
    group('DP0_DP_MSA_MISC', 'DP_MSA_MISC1', 0xff)
    group('DP0_DP_VID_TIMING', 'DP_VID_M_N_DOUBLE_BUFFER_MODE DP_VID_M_DIV DP_VID_N_DIV', 0xff003010)
    group('DP0_DP_VID_TIMING', 'DP_VID_M_N_GEN_EN DP_VID_N_MUL', 0xd00)
    group('DP0_DP_VID_N', 'DP_VID_N', 0xffffff)
    group('DP0_DP_VID_M', 'DP_VID_M', 0xffffff)
    group('DP0_DP_VID_STREAM_CNTL', 'DP_VID_STREAM_ENABLE DP_VID_STREAM_STATUS', 0x10001)
    group('DP0_DP_VID_STREAM_CNTL', 'DP_VID_STREAM_DIS_DEFER', 0x300)
    group('DP0_DP_STEER_FIFO', 'DP_STEER_FIFO_RESET DP_STEER_OVERFLOW_FLAG DP_TU_OVERFLOW_FLAG', 0x111)
    group('DP0_DP_SEC_CNTL', 'DP_SEC_STREAM_ENABLE DP_SEC_ASP_ENABLE DP_SEC_ATP_ENABLE DP_SEC_AIP_ENABLE '
          'DP_SEC_ACM_ENABLE '+' '.join('DP_SEC_GSP'+str(n)+'_ENABLE' for n in range(8))+' DP_SEC_MPG_ENABLE', 0x1ff11111)
    group('DP0_DP_SEC_CNTL1', 'DP_SEC_ISRC_ENABLE DP_SEC_GSP0_SEND DP_SEC_GSP0_SEND_PENDING DP_SEC_GSP0_SEND_ANY_LINE', 0x161)
    group('DP0_DP_SEC_CNTL2', ' '.join('DP_SEC_GSP'+str(n)+'_'+s for n in range(1,8) for s in ['SEND','SEND_PENDING'])+' DP_SEC_GSP11_PPS', 0x13333333)
    for n in range(8,12):
        group(f'DP0_DP_GSP{n}_CNTL', f'DP_MSO_SEC_GSP{n}_ENABLE '+' '.join(f'DP_SEC_GSP{n}_'+s for s in
              ['ENABLE','SEND_IN_IDLE','SEND','SEND_ANY_LINE','SEND_PENDING','SEND_ACTIVE']), 0x31df)
    group('DP0_DP_SEC_METADATA_TRANSMISSION', 'DP_SEC_METADATA_PACKET_ENABLE DP_SEC_MSO_METADATA_PACKET_ENABLE', 0xf1)
    group('DP0_DP_SEC_FRAMING4', 'DP_SST_SDP_SPLITTING', 1)
    group('DP0_DP_DSC_CNTL', 'DP_DSC_MODE', 1)
    group('DP0_DP_DB_CNTL', 'DP_DB_PENDING DP_DB_LOCK DP_VUPDATE_DB_PENDING', 0x8101)
    group('DP0_DP_MSA_VBID_MISC', 'DP_MSA_MISC1_STEREOSYNC_OVERRIDE_EN DP_VBID1_OVERRIDE_EN DP_VBID2_OVERRIDE_EN', 0x3010)
    group('OTG0_OTG_CONTROL', 'OTG_MASTER_EN OTG_START_POINT_CNTL OTG_FIELD_NUMBER_CNTL '
          'OTG_CURRENT_MASTER_EN_STATE OTG_OUT_MUX', 0x313001)
    group('OTG0_OTG_CLOCK_CONTROL', 'OTG_CLOCK_ON OTG_SOFT_RESET', 0x110)
    group('OTG0_OTG_INTERLACE_CONTROL', 'OTG_INTERLACE_ENABLE OTG_INTERLACE_FORCE_NEXT_FIELD', 0x30001)
    for axis in ['H','V']:
        group(f'OTG0_OTG_{axis}_TOTAL', f'OTG_{axis}_TOTAL', 0x7fff)
        group(f'OTG0_OTG_{axis}_BLANK_START_END', f'OTG_{axis}_BLANK_START OTG_{axis}_BLANK_END', 0x7fff7fff)
        group(f'OTG0_OTG_{axis}_SYNC_A', f'OTG_{axis}_SYNC_A_START OTG_{axis}_SYNC_A_END', 0x7fff7fff)
    group('OTG0_OTG_H_SYNC_A_CNTL', 'OTG_H_SYNC_A_POL OTG_COMP_SYNC_A_EN OTG_H_SYNC_A_CUTOFF', 0x30001)
    group('OTG0_OTG_V_SYNC_A_CNTL', 'OTG_V_SYNC_A_POL OTG_V_SYNC_MODE', 0x101)
    return {'sources': sources, 'registers': table, 'fields': fields}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', required=True, type=Path)
    ap.add_argument('--discovery', required=True, type=Path)
    args = ap.parse_args(); out = args.output.resolve()
    if out.exists(): ap.error('Use a fresh output directory')
    out.mkdir(parents=True)
    zig = ROOT/'.tools/zig-x86_64-windows-0.17.0/zig.exe'
    report = {'success': False, 'gpu_executed': False, 'remote_executed': False,
              'target_executed': False, 'commands': [], 'source_files': {p: sha(ROOT/p) for p in set(OWNED+LIBRARY)},
              'compiler_sha256': sha(zig), 'discovery': {'path': str(args.discovery.resolve()), 'sha256': sha(args.discovery)}}

    def run(command, negative=False):
        result = subprocess.run([str(x) for x in command], cwd=ROOT, capture_output=True, text=True, timeout=120)
        report['commands'].append({'args': [str(x) for x in command], 'exit_code': result.returncode,
                                   'negative_control': negative, 'stdout': result.stdout, 'stderr': result.stderr})
        if negative:
            assert result.returncode == 1 and re.search(r'DP stream assertions, [1-9]\d* failures', result.stdout), result
        elif result.returncode: raise RuntimeError(result.stdout+result.stderr)

    try:
        report['oracle'] = oracle()
        common = [zig, 'c++', '-std=c++11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-Iinclude']
        for kind, flags in [('strict', []), ('ubsan', ['-fsanitize=undefined', '-fno-sanitize-recover=all'])]:
            exe = out/(kind+'.exe')
            run([*common, *flags, *LIBRARY, 'tests/dp_stream_tests.cpp', '-o', exe]); run([exe, args.discovery.resolve()])
        source = (ROOT/'src/dp_stream.cpp').read_text()
        mutations = {
            'missing-post-write-guard': ('if(!io_.valid(io_.context))return Status::DeviceUnavailable;\n    if(!writeOk)', 'if(!writeOk)'),
            'missing-stop-wait': ('s=wait(Register::Stream,0x10001,0,p,r);if(s!=Status::Ok)return finish(s,r);r.streamStopped=true;', 'r.streamStopped=true;'),
        }
        for name, (old, new) in mutations.items():
            assert source.count(old) == 1
            mutant = out/(name+'.cpp'); mutant.write_text(source.replace(old, new)); exe = out/(name+'.exe')
            run([*common, mutant, *LIBRARY[1:], 'tests/dp_stream_tests.cpp', '-o', exe]); run([exe], negative=True)
        kernel = [zig, 'c++', '-target', 'x86_64-macos.15.0', '-std=c++11', '-mkernel', '-fapple-kext',
                  '-DKERNEL', '-DKERNEL_EXTENSION', '-nostdinc', '-nostdlibinc', '-I.research/MacKernelSDK/Headers',
                  '-Iinclude', '-fno-exceptions', '-fno-rtti', '-fno-builtin', '-fno-common', '-fno-stack-protector',
                  '-mno-red-zone', '-mno-mmx', '-mno-sse', '-mno-sse2', '-Wall', '-Wextra', '-Werror',
                  '-Wframe-larger-than=2048', '-O2']
        before = out/'kernel-before.d'
        run([*kernel, '-M', '-MT', 'dp_stream.o', '-MF', before, 'src/dp_stream.cpp'])

        def dependencies(path):
            paths = path.read_text().replace('\\\n', ' ').split(':', 1)[1].split()
            return {p: sha(ROOT/p) for p in paths}

        report['kernel_dependencies'] = dependencies(before)
        run([*kernel, '-MD', '-MT', 'dp_stream.o', '-MF', out/'kernel-compiled.d', '-c', 'src/dp_stream.cpp', '-o', out/'dp_stream.o'])
        assert dependencies(out/'kernel-compiled.d') == report['kernel_dependencies']
        report['artifacts'] = {p.name: sha(p) for p in out.iterdir() if p.suffix in ('.exe', '.o')}
        report['success'] = True
    finally:
        report['inputs_unchanged'] = all(sha(ROOT/p) == h for p, h in report['source_files'].items())
        report['inputs_unchanged'] &= sha(args.discovery) == report['discovery']['sha256']
        report['inputs_unchanged'] &= all(sha(ROOT/p) == h for p, h in report.get('kernel_dependencies', {}).items())
        report['success'] &= report['inputs_unchanged']
        (out/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    if not report['success']: raise RuntimeError('DP stream validation failed')
    print(json.dumps({'success': True, 'report_sha256': sha(out/'report.json')}))


if __name__ == '__main__': main()
