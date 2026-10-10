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
OWNED = ['include/hackynavi/boot_display_backend.hpp', 'src/boot_display_backend.cpp',
         'tests/boot_display_backend_tests.cpp', 'docs/boot-display-backend.md',
         'scripts/check-boot-display-backend-local.py']
DEPENDENCIES = ['include/hackynavi/dmcub.hpp', 'include/hackynavi/rom.hpp',
                'include/hackynavi/discovery.hpp', 'src/discovery.cpp', 'src/rom.cpp', 'src/dmcub.cpp']
UPSTREAM = ['dcn_3_2_0_offset.h', 'dcn_3_2_0_sh_mask.h', 'dcn10_link_encoder.c',
            'dcn32_dio_link_encoder.c', 'dcn32_dio_stream_encoder.c', 'dcn32_optc.c',
            'dcn10_mpc.c', 'dcn10_hwseq.c', 'dcn20_hwseq.c', 'link_dpms.c', 'link_hwss_dio.c']


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def register_oracle():
    raw = (ROOT / '.research/linux/dcn_3_2_0_offset.h').read_text()
    offsets = {name: int(value, 0) for name, value in re.findall(r'^#define (reg\w+)\s+(0x[\da-f]+|\d+)\s*$', raw, re.M)}
    source = (ROOT / 'src/boot_display_backend.cpp').read_text()
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
        body = re.search(r'const uint16_t ' + group + r'\[\w+\]=\{([^}]+)\}', source).group(1)
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
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--rom', type=Path, required=True)
    args = ap.parse_args()
    out = args.output.resolve()
    if out.exists():
        ap.error('Use a fresh evidence directory')
    out.mkdir(parents=True)
    zig = ROOT / '.tools/zig-x86_64-windows-0.17.0/zig.exe'
    bound = OWNED + DEPENDENCIES + ['.research/linux/' + x for x in UPSTREAM]
    report = {'success': False, 'gpu_executed': False, 'remote_executed': False,
              'source_files': {p: sha(ROOT / p) for p in bound}, 'compiler_sha256': sha(zig),
              'rom': {'path': str(args.rom), 'bytes': args.rom.stat().st_size, 'sha256': sha(args.rom)},
              'commands': [], 'validation_scope': 'Numeric register protocol tests plus actual supplied ROM parsing; no target register observation'}

    def run(command):
        result = subprocess.run([str(x) for x in command], cwd=ROOT, capture_output=True, text=True, timeout=120)
        report['commands'].append({'args': [str(x) for x in command], 'exit_code': result.returncode,
                                   'stdout': result.stdout, 'stderr': result.stderr})
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    try:
        report['register_oracle'] = register_oracle()
        common = [zig, 'c++', '-std=c++11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-Iinclude']
        inputs = ['src/boot_display_backend.cpp', 'src/discovery.cpp', 'src/rom.cpp', 'src/dmcub.cpp',
                  'tests/boot_display_backend_tests.cpp']
        for name, extra in [('strict', []), ('ubsan', ['-fsanitize=undefined', '-fno-sanitize-recover=all'])]:
            exe = out / (name + '.exe')
            run([*common, *extra, *inputs, '-o', exe])
            run([exe, args.rom.resolve()])
        kernel = [zig, 'c++', '-target', 'x86_64-macos.15.0', '-std=c++11', '-mkernel', '-fapple-kext',
                  '-DKERNEL', '-DKERNEL_EXTENSION', '-nostdinc', '-nostdlibinc', '-I', '.research/MacKernelSDK/Headers',
                  '-I', 'include', '-fno-exceptions', '-fno-rtti', '-fno-builtin', '-fno-common',
                  '-fno-stack-protector', '-mno-red-zone', '-mno-mmx', '-mno-sse', '-mno-sse2',
                  '-Wall', '-Wextra', '-Werror', '-Wframe-larger-than=2048', '-O2']
        run([*kernel, '-M', '-MT', 'backend.o', '-MF', out / 'kernel-before.d', 'src/boot_display_backend.cpp'])
        def dependencies(path):
            text = path.read_text().replace('\\\n', ' ')
            paths = text.split(':', 1)[1].split()
            return {p: sha(ROOT / p) for p in paths}
        report['kernel_dependencies'] = dependencies(out / 'kernel-before.d')
        run([*kernel, '-MD', '-MT', 'backend.o', '-MF', out / 'kernel-compiled.d', '-c',
             'src/boot_display_backend.cpp', '-o', out / 'backend-kernel.o'])
        assert report['kernel_dependencies'] == dependencies(out / 'kernel-compiled.d')
        report['artifacts'] = {p.name: sha(p) for p in out.iterdir() if p.suffix in ('.exe', '.o')}
        report['success'] = True
    finally:
        report['inputs_unchanged'] = all(sha(ROOT / p) == h for p, h in report['source_files'].items()) and sha(args.rom) == report['rom']['sha256']
        report['success'] &= report['inputs_unchanged']
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    if not report['success']:
        raise RuntimeError('Focused validation failed')
    freeze = {'source_files': report['source_files'], 'report_sha256': sha(out / 'report.json'),
              'kernel_dependencies': report['kernel_dependencies'], 'no_target_or_gpu_execution': True}
    (out / 'source-freeze.json').write_text(json.dumps(freeze, indent=2) + '\n')
    print(json.dumps({'freeze_sha256': sha(out / 'source-freeze.json'), 'report_sha256': sha(out / 'report.json')}))


if __name__ == '__main__':
    main()
