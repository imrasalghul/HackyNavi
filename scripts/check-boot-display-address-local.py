#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bind the archived 9eda capture, register definitions and local decoder tests. No GPU/SSH."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OWNED = ['src/boot_display.cpp', 'include/hackynavi/boot_display.hpp',
         'tests/boot_display_tests.cpp', 'tests/fixtures/boot_display_9eda.hpp',
         'scripts/check-boot-display-address-local.py', 'docs/boot-display-address-audit.md']
DEPENDENCIES = ['src/discovery.cpp', 'src/rom.cpp', 'src/memory_layout.cpp',
                'src/boot_display_stop.cpp', 'include/hackynavi/boot_display_stop.hpp',
                'tests/boot_display_stop_tests.cpp']
UPSTREAM = ['dcn_3_2_0_offset.h', 'dcn_3_2_0_sh_mask.h', 'dcn32_hubp.c',
            'dcn30_hubp.c', 'dcn20_hubp.c', 'dcn30_hubbub.c']
CAPTURE_SHA = 'e371164d350d1bbad52d0cf76fd235956a860933d7f356c62dd82413147162c5'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_capture(path):
    assert sha(path) == CAPTURE_SHA, 'Expected exact archived 9eda device.plist'
    found = {}

    def walk(value):
        if isinstance(value, dict):
            for key, entry in value.items():
                if key in ('BootDisplay', 'MemoryLayout'):
                    assert key not in found or found[key] == entry, 'Contradictory duplicate property'
                    found[key] = entry
                else:
                    walk(entry)
        elif isinstance(value, list):
            for entry in value:
                walk(entry)

    walk(plistlib.loads(path.read_bytes()))
    display, memory = found['BootDisplay'], found['MemoryLayout']
    assert display['SnapshotReadComplete'] and not display['ReservationUseAllowed']
    raw = display['RawSnapshot']
    assert raw['CurrentHardwareAuthority'] is False
    assert [p['Pipe'] for p in raw['Pipes']] == [0, 1, 2, 3]
    assert all(p['WindowRead'] for p in raw['Pipes'])
    fixture = (ROOT / 'tests/fixtures/boot_display_9eda.hpp').read_text()
    assert CAPTURE_SHA in fixture

    def numbers(name):
        match = re.search(r'\b' + name + r'(?:\[\d+\])*\s*=\s*\{(.*?)\};', fixture, re.S)
        assert match, name
        return [int(x, 16) for x in re.findall(r'0x([0-9a-f]+)U', match.group(1))]

    for name, key in [('globalOffsets', 'GlobalRegisterOffsets'), ('globalValues', 'GlobalRegisterValues'),
                      ('vgaOffsets', 'VgaRegisterOffsets'), ('vgaValues', 'VgaRegisterValues')]:
        assert numbers(name) == raw[key], name
    for name, key in [('pipeOffsets', 'RegisterOffsets'), ('pipeValues', 'RegisterValues')]:
        assert numbers(name) == [v for p in raw['Pipes'] for v in p[key]], name
    for name, key in [('powerValues', 'PowerStatus'), ('powerConfigs', 'PowerConfig')]:
        assert numbers(name) == [p[key] for p in raw['Pipes']], name
    keys = ['FramebufferBaseRaw', 'FramebufferTopRaw', 'FramebufferOffsetRaw',
            'VRAMBytes', 'BAR0PhysicalAddress', 'BAR0Bytes']
    assert numbers('memory') == [memory[k] for k in keys]
    return {'path': str(path), 'sha256': sha(path), 'bytes': path.stat().st_size,
            'fixture_all_registers_and_memory_match': True,
            'raw_capture_complete': True, 'target_classification_permitted': False}


def source_oracle():
    manifest = json.loads((ROOT / '.research/linux/boot_display_sources.json').read_text())
    files = {entry['local']: entry for entry in manifest['files']}
    result = {}
    for name in UPSTREAM:
        path = ROOT / '.research/linux' / name
        assert sha(path) == files[name]['sha256'], name
        result[name] = {**files[name], 'url': 'https://github.com/torvalds/linux/blob/' +
                        manifest['commit'] + '/' + files[name]['path']}
    enum_source = ROOT / '.research/linux/navi10_enum.h'
    enum_receipt = json.loads(enum_source.with_suffix('.h.source.json').read_text())
    assert sha(enum_source) == enum_receipt['sha256']
    result['navi10_enum.h'] = enum_receipt
    masks = (ROOT / '.research/linux/dcn_3_2_0_sh_mask.h').read_text()
    fields = {'HUBP0_DCSURF_TILING_CONFIG__DIM_TYPE_MASK': 0x180,
              'HUBP0_DCSURF_TILING_CONFIG__SW_MODE_MASK': 0x1f,
              'HUBP0_DCHUBP_CNTL__HUBP_VTG_SEL_MASK': 0xf0,
              'HUBP0_DCHUBP_CNTL__HUBP_TTU_DISABLE_MASK': 0x1000,
              'HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK': 0x100,
              'HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_DELAY_MASK': 0x3ff00000,
              'HUBPREQ0_DCSURF_FLIP_CONTROL2__SURFACE_INUSE_RAED_NO_LATCH_MASK': 0x1000}
    for name, value in fields.items():
        assert int(re.search(r'^#define ' + name + r'\s+(0x[0-9A-Fa-f]+)L', masks, re.M)[1], 16) == value
    enum = enum_source.read_text()
    for name, value in [('DIM_TYPE_1D', 0), ('DIM_TYPE_2D', 1), ('DIM_TYPE_3D', 2),
                        ('DIM_TYPE_RESERVED', 3), ('SURFACE_INUSE_IS_LATCHED', 0)]:
        assert int(re.search(r'\b' + name + r'\s*=\s*(0x[0-9a-fA-F]+)', enum)[1], 16) == value
    return {'files': result, 'checked_fields': fields,
            'physical_TLB0_semantics_established': False}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', required=True, type=Path)
    ap.add_argument('--capture', required=True, type=Path)
    ap.add_argument('--discovery', required=True, type=Path)
    args = ap.parse_args()
    out = args.output.resolve()
    if out.exists():
        ap.error('Use a fresh output directory')
    out.mkdir(parents=True)
    zig = ROOT / '.tools/zig-x86_64-windows-0.17.0/zig.exe'
    report = {'success': False, 'target_execution': False, 'gpu_executed': False, 'remote_executed': False,
              'source_files': {p: sha(ROOT / p) for p in OWNED + DEPENDENCIES},
              'compiler_sha256': sha(zig), 'commands': [],
              'discovery': {'path': str(args.discovery.resolve()), 'sha256': sha(args.discovery)}}

    def run(command):
        response = subprocess.run([str(x) for x in command], cwd=ROOT, capture_output=True,
                                  text=True, timeout=180)
        report['commands'].append({'args': [str(x) for x in command], 'exit_code': response.returncode,
                                   'stdout': response.stdout, 'stderr': response.stderr})
        if response.returncode:
            raise RuntimeError(response.stdout + response.stderr)

    try:
        report['capture'] = verify_capture(args.capture.resolve())
        report['source_oracle'] = source_oracle()
        common = [zig, 'c++', '-std=c++11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-Iinclude']
        library = ['src/boot_display.cpp', 'src/discovery.cpp', 'src/rom.cpp', 'src/memory_layout.cpp']
        for suite, extra_source in [('capture', ['tests/boot_display_tests.cpp']),
                                     ('stop', ['src/boot_display_stop.cpp', 'tests/boot_display_stop_tests.cpp'])]:
            for kind, flags in [('strict', []), ('ubsan', ['-fsanitize=undefined', '-fno-sanitize-recover=all'])]:
                exe = out / (suite + '-' + kind + '.exe')
                run([*common, *flags, *library, *extra_source, '-o', exe])
                run([exe, args.discovery.resolve()])
        kernel = [zig, 'c++', '-target', 'x86_64-macos.15.0', '-std=c++11', '-mkernel', '-fapple-kext',
                  '-DKERNEL', '-DKERNEL_EXTENSION', '-nostdinc', '-nostdlibinc',
                  '-I.research/MacKernelSDK/Headers', '-Iinclude', '-fno-exceptions', '-fno-rtti',
                  '-fno-builtin', '-fno-common', '-fno-stack-protector', '-mno-red-zone', '-mno-mmx',
                  '-mno-sse', '-mno-sse2', '-Wall', '-Wextra', '-Werror', '-Wframe-larger-than=2048', '-O2']
        report['kernel_dependencies'] = {}
        for source in ['src/boot_display.cpp', 'src/boot_display_stop.cpp']:
            name = Path(source).stem
            dep = out / (name + '.d')
            run([*kernel, '-M', '-MT', name, '-MF', dep, source])
            paths = dep.read_text().replace('\\\n', ' ').split(':', 1)[1].split()
            report['kernel_dependencies'].update({p: sha(ROOT / p) for p in paths})
            run([*kernel, '-c', source, '-o', out / (name + '.o')])
        report['artifacts'] = {p.name: sha(p) for p in out.iterdir() if p.suffix in ('.exe', '.o')}
        report['success'] = True
    finally:
        report['inputs_unchanged'] = all(sha(ROOT / p) == h for p, h in report['source_files'].items())
        report['inputs_unchanged'] &= all(sha(ROOT / p) == h for p, h in report.get('kernel_dependencies', {}).items())
        report['success'] &= report['inputs_unchanged']
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    if not report['success']:
        raise RuntimeError('Focused validation failed')
    print(json.dumps({'success': True, 'report_sha256': sha(out / 'report.json')}))


if __name__ == '__main__':
    main()
