#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Independent OPP register/field oracle, focused protocol tests, real kernel compile."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OWNED = ['tests/boot_opp_blank_tests.cpp', 'tests/fixtures/boot_backend_fixture.hpp',
         'scripts/check-boot-opp-blank-local.py', 'docs/boot-opp-blank-review.md', 'docs/boot-opp-blank.md']
PRODUCTION = ['include/hackynavi/boot_opp_blank.hpp', 'src/boot_opp_blank.cpp',
              'include/hackynavi/boot_display_backend.hpp', 'src/boot_display_backend.cpp',
              'src/discovery.cpp', 'src/rom.cpp', 'src/dmcub.cpp']
UPSTREAM = ['dcn_3_2_0_offset.h', 'dcn_3_2_0_sh_mask.h', 'dcn32_hwseq.c',
            'dcn20_opp.c', 'dcn10_optc.c', 'dcn20_hwseq.c', 'hw_shared.h']


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def oracle():
    raw = (ROOT / '.research/linux/dcn_3_2_0_offset.h').read_text()
    defs = {k: int(v, 0) for k, v in re.findall(r'^#define (reg\w+)\s+(0x[\da-f]+|\d+)\s*$', raw, re.M)}
    source = (ROOT / 'src/boot_opp_blank.cpp').read_text()
    test = (ROOT / 'tests/boot_opp_blank_tests.cpp').read_text()
    def array(text):
        return [int(x.strip(), 0) for x in re.search(r'const uint16_t offsets\[[^]]*\]=\{([^}]+)\}', text).group(1).split(',')]
    actual = array(source)
    assert actual == array(test)
    names = ['DPG{}_DPG_CONTROL', 'DPG{}_DPG_DIMENSIONS', 'DPG{}_DPG_OFFSET_SEGMENT',
             'DPG{}_DPG_COLOUR_B_CB', 'DPG{}_DPG_COLOUR_G_Y', 'DPG{}_DPG_COLOUR_R_CR',
             'DPG{}_DPG_STATUS', 'OPP_PIPE{}_OPP_PIPE_CONTROL', 'FMT{}_FMT_CONTROL',
             'OTG{}_OTG_H_BLANK_START_END', 'OTG{}_OTG_V_BLANK_START_END', 'OTG{}_OTG_CONTROL', 'OTG{}_OTG_CLOCK_CONTROL']
    assert len(actual) == len(names) == 13
    addresses = []
    for i, name in enumerate(names):
        for instance in range(4):
            symbol = 'reg' + name.format(instance)
            assert actual[i] + instance * (0x80 if i >= 9 else 0x5a) == defs[symbol], symbol
            assert defs[symbol + '_BASE_IDX'] == 2
            addresses.append({'symbol': symbol, 'dword': defs[symbol], 'base_index': 2})
    mask_raw = (ROOT / '.research/linux/dcn_3_2_0_sh_mask.h').read_text()
    masks = {k: int(v, 0) for k, v in re.findall(r'^#define (\w+)\s+(0x[\dA-Fa-f]+|\d+)[UL]*\s*$', mask_raw, re.M)}
    def m(register, field):
        return masks[register + '__' + field + '_MASK']
    prefix = 'DPG0_DPG_CONTROL'
    control_fields = ['DPG_EN', 'DPG_MODE', 'DPG_DYNAMIC_RANGE', 'DPG_BIT_DEPTH', 'DPG_VRES', 'DPG_HRES', 'DPG_FIELD_POLARITY']
    control = 0
    for field in control_fields:
        control |= m(prefix, field)
    assert control == 0x01ff3171 and 'control&~0x01ff3171u' in source
    assert m(prefix, 'DPG_EN') | m(prefix, 'DPG_MODE') == 0x71 and 'control&~0x71u' in source
    assert masks[prefix + '__DPG_MODE__SHIFT'] == 4
    enum_raw = (ROOT / '.research/linux/hw_shared.h').read_text()
    values = re.search(r'enum test_pattern_mode\s*\{([^}]+)\}', enum_raw).group(1).split(',')
    assert values[0].strip() == 'TEST_PATTERN_MODE_COLORSQUARES_RGB = 0'
    assert values[4].strip() == 'TEST_PATTERN_MODE_HORIZONTALBARS'
    assert (4 << masks[prefix + '__DPG_MODE__SHIFT']) | m(prefix, 'DPG_EN') == 0x41
    assert m('DPG0_DPG_DIMENSIONS', 'DPG_ACTIVE_WIDTH') == 0x3fff0000
    assert m('DPG0_DPG_DIMENSIONS', 'DPG_ACTIVE_HEIGHT') == 0x3fff
    assert m('DPG0_DPG_STATUS', 'DPG_DOUBLE_BUFFER_PENDING') == 1
    assert m('OPP_PIPE0_OPP_PIPE_CONTROL', 'OPP_PIPE_CLOCK_EN') | m('OPP_PIPE0_OPP_PIPE_CONTROL', 'OPP_PIPE_CLOCK_ON') == 3
    fmt_fields = ['FMT_STEREOSYNC_OVERRIDE', 'FMT_PIXEL_ENCODING', 'FMT_SUBSAMPLING_MODE',
                  'FMT_SUBSAMPLING_ORDER', 'FMT_CBCR_BIT_REDUCTION_BYPASS', 'FMT_DOUBLE_BUFFER_REG_UPDATE_PENDING']
    fmt = 0
    for field in fmt_fields:
        fmt |= m('FMT0_FMT_CONTROL', field)
    assert fmt == 0x013f0001 and 'fmt&0x013f0001u' in source
    for axis in ('H', 'V'):
        register = f'OTG0_OTG_{axis}_BLANK_START_END'
        assert m(register, f'OTG_{axis}_BLANK_START') == 0x7fff
        assert m(register, f'OTG_{axis}_BLANK_END') == 0x7fff0000
    return {'register_addresses': addresses, 'control_allowed_mask': control,
            'control_update_mask': 0x71, 'horizontal_bars_mode': 4,
            'format_rejected_mask': fmt, 'scope': 'Source-address and field oracle; no GPU measurement'}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    out = args.output.resolve()
    if out.exists():
        ap.error('Use a fresh evidence directory')
    out.mkdir(parents=True)
    zig = ROOT / '.tools/zig-x86_64-windows-0.17.0/zig.exe'
    bound = OWNED + PRODUCTION + ['.research/linux/' + p for p in UPSTREAM]
    report = {'success': False, 'source_files': {p: sha(ROOT / p) for p in bound},
              'compiler_sha256': sha(zig), 'commands': [], 'gpu_executed': False, 'remote_executed': False}

    def run(command):
        result = subprocess.run([str(x) for x in command], cwd=ROOT, capture_output=True, text=True, timeout=120)
        report['commands'].append({'args': [str(x) for x in command], 'exit_code': result.returncode,
                                   'stdout': result.stdout, 'stderr': result.stderr})
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    try:
        report['oracle'] = oracle()
        for name, extra in [('strict', []), ('ubsan', ['-fsanitize=undefined', '-fno-sanitize-recover=all'])]:
            exe = out / (name + '.exe')
            run([zig, 'c++', '-std=c++11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-Iinclude',
                 *extra, *[p for p in PRODUCTION if p.endswith('.cpp')], 'tests/boot_opp_blank_tests.cpp', '-o', exe])
            run([exe])
        flags = [zig, 'c++', '-target', 'x86_64-macos.15.0', '-std=c++11', '-mkernel', '-fapple-kext',
                 '-DKERNEL', '-DKERNEL_EXTENSION', '-nostdinc', '-nostdlibinc', '-I', '.research/MacKernelSDK/Headers',
                 '-Iinclude', '-fno-exceptions', '-fno-rtti', '-fno-builtin', '-fno-common', '-fno-stack-protector',
                 '-mno-red-zone', '-mno-mmx', '-mno-sse', '-mno-sse2', '-Wall', '-Wextra', '-Werror',
                 '-Wframe-larger-than=2048', '-O2']
        run([*flags, '-M', '-MT', 'opp.o', '-MF', out / 'before.d', 'src/boot_opp_blank.cpp'])
        def deps(path):
            return {p: sha(ROOT / p) for p in path.read_text().replace('\\\n', ' ').split(':', 1)[1].split()}
        report['kernel_dependencies'] = deps(out / 'before.d')
        run([*flags, '-MD', '-MT', 'opp.o', '-MF', out / 'compiled.d', '-c', 'src/boot_opp_blank.cpp', '-o', out / 'opp.o'])
        assert report['kernel_dependencies'] == deps(out / 'compiled.d')
        report['artifacts'] = {p.name: sha(p) for p in out.iterdir() if p.suffix in ('.exe', '.o')}
        report['success'] = True
    finally:
        report['inputs_unchanged'] = all(sha(ROOT / p) == h for p, h in report['source_files'].items())
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
