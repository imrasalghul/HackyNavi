#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Pinned Linux ABI, offline typed TA lifecycle, and real kernel-header checks.

No remote execution, firmware execution, GPU access, or inherited-state proof.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
LINUX = '3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0'
SOURCES = ['src/rom.cpp', 'src/firmware.cpp', 'src/discovery.cpp', 'src/ip_layout.cpp',
           'src/memory_layout.cpp', 'src/vram_allocator.cpp', 'src/vram_transfer.cpp',
           'src/psp.cpp', 'src/psp_ring.cpp']
OWNED = ['include/hackynavi/psp_display_ta.hpp', 'src/psp_display_ta.cpp',
         'include/hackynavi/psp_ring.hpp', 'src/psp_ring.cpp',
         'include/hackynavi/psp_ta.hpp', 'src/psp_ta.cpp',
         'tests/psp_display_ta_tests.cpp', 'tests/psp_ta_tests.cpp',
         'tests/psp_ring_tests.cpp', 'tests/psp_ring_fixture.hpp',
         'docs/psp-display-ta-protocol.md', 'scripts/check-psp-display-ta-local.py']
UPSTREAM = ['.research/inherited-hdmi/' + p for p in
            ['hdcp_psp.h', 'hdcp_psp.c', 'amdgpu_dm_hdcp.c', 'hdcp-fetch.json']]
UPSTREAM += ['.research/linux/' + p for p in ['link_dpms.c', 'dc_resource.c', 'dcn32_resource.c']]
HEADER = '.research/inherited-hdmi/hdcp_psp.h'
HEADER_SHA = '684213a47d01468dada6590b1edf94ed325e9a903fc435929832635863aeecb3'
FIRMWARE = '.research/firmware/amdgpu/psp_13_0_0_ta.bin'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    if out.exists():
        parser.error('Use a fresh evidence directory')
    out.mkdir(parents=True)
    compiler = ROOT / '.tools/zig-x86_64-windows-0.17.0/zig.exe'
    bound = list(dict.fromkeys(OWNED + SOURCES + UPSTREAM + [FIRMWARE]))
    report = {'success': False, 'target_executed': False, 'remote_executed': False,
              'gpu_executed': False, 'source_files': {p: sha(ROOT / p) for p in bound},
              'compiler_sha256': sha(compiler), 'commands': [], 'kernel_dependencies': {},
              'scope': 'Actual pinned ABI oracle, offline lifecycle/fault cases, real kernel-header objects; no hardware proof'}

    def run(command):
        result = subprocess.run([str(x) for x in command], cwd=ROOT, capture_output=True,
                                text=True, timeout=120)
        report['commands'].append({'args': [str(x) for x in command],
                                   'exit_code': result.returncode,
                                   'stdout': result.stdout, 'stderr': result.stderr})
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def dependencies(path):
        names = path.read_text().replace('\\\n', ' ').split(':', 1)[1].split()
        return {p: sha(ROOT / p) for p in names}

    try:
        assert sha(ROOT / HEADER) == HEADER_SHA
        assert sha(ROOT / FIRMWARE) == 'ac2454a36618d8568e798c6000bf3e19fdabf46d46038b92635adc3cb01bd198'
        for item in json.loads((ROOT / UPSTREAM[3]).read_text()):
            assert f'/{LINUX}/' in item['url']
            assert sha(ROOT / item['path']) == item['sha256']
        report['abi_source'] = {
            'linux_commit': LINUX, 'path': HEADER, 'sha256': HEADER_SHA,
            'url': f'https://raw.githubusercontent.com/torvalds/linux/{LINUX}/drivers/gpu/drm/amd/display/modules/hdcp/hdcp_psp.h',
            'verification': 'Local bytes equal the pinned upstream header; C++ tests include this unchanged header as their wire oracle',
            'display_mapping_sources': UPSTREAM[2:3] + UPSTREAM[4:]}
        common = [compiler, 'c++', '-std=c++11', '-O2', '-Wall', '-Wextra',
                  '-Wpedantic', '-Werror', '-Iinclude', '-I.research/inherited-hdmi']
        for mode, flags in [('strict', []), ('ubsan', ['-fsanitize=undefined', '-fno-sanitize-recover=all'])]:
            for name, extra, arguments in [
                ('display-ta', ['src/psp_display_ta.cpp', 'tests/psp_display_ta_tests.cpp'], []),
                ('existing-ta', ['src/psp_ta.cpp', 'tests/psp_ta_tests.cpp'], [FIRMWARE]),
                ('existing-ring', ['tests/psp_ring_tests.cpp'], [])]:
                exe = out / f'{name}-{mode}.exe'
                run([*common, *flags, *SOURCES, *extra, '-o', exe])
                run([exe, *arguments])
        kernel = [compiler, 'c++', '-target', 'x86_64-macos.15.0', '-std=c++14',
                  '-mkernel', '-fapple-kext', '-DKERNEL', '-DKERNEL_EXTENSION',
                  '-nostdinc', '-nostdlibinc', '-I.research/MacKernelSDK/Headers', '-Iinclude',
                  '-fno-exceptions', '-fno-rtti', '-fno-builtin', '-fno-common',
                  '-fno-stack-protector', '-mno-red-zone', '-mno-mmx', '-mno-sse', '-mno-sse2',
                  '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                  '-Wframe-larger-than=2048', '-O2', '-fstack-usage']
        for source in ['src/psp_display_ta.cpp', 'src/psp_ring.cpp', 'src/psp_ta.cpp']:
            name = Path(source).stem
            before, after = out / (name + '-before.d'), out / (name + '-compiled.d')
            run([*kernel, '-M', '-MT', 'kernel.o', '-MF', before, source])
            report['kernel_dependencies'][source] = dependencies(before)
            run([*kernel, '-Xclang', '-stack-usage-file', '-Xclang', out / (name + '.su'),
                 '-MD', '-MT', 'kernel.o', '-MF', after, '-c', source, '-o', out / (name + '.o')])
            assert report['kernel_dependencies'][source] == dependencies(after)
        report['kernel_stack_usage'] = {}
        for stack in out.glob('*.su'):
            frames = [int(line.split('\t')[-2]) for line in stack.read_text().splitlines() if line]
            assert frames and max(frames) <= 2048
            report['kernel_stack_usage'][stack.name] = {'maximum_bytes': max(frames), 'sha256': sha(stack)}
        assert len(report['kernel_stack_usage']) == 3
        report['artifacts'] = {p.name: sha(p) for p in out.iterdir() if p.suffix in ('.exe', '.o', '.su')}
        report['semantic_cases'] = [
            'Unchanged pinned Linux HDCP/DTM struct layout and byte-exact full shared messages',
            'Both actual loaded TA records leased exclusively; raw invoke/unload blocked',
            'Previously raw-invoked TA records cannot become a fresh typed owner',
            'Opaque owner-issued topology/authentication tokens; actual returned zero/nonzero session handles',
            'Stale/cross-owner tokens, duplicate returned sessions and topology exclusivity',
            'Allocator/Io/State/inputs/outputs/scratch aliases rejected without I/O or object mutation',
            'PSP ACK and application status kept separate; unchanged failure sentinel is not success',
            'Publication, response, guard and timeout ambiguity retains both loaded TAs and faults native callback',
            'Late PSP observation cannot revive typed owner; deadline checked after shared-response validation',
            'Owned auth destroys and topology removes required before lease release',
            'Existing actual-image TA and PSP ring regressions under strict and UBSan',
            'No inherited sessions adopted, no sink authentication handshake or encryption enable implemented']
        report['success'] = True
    finally:
        report['inputs_unchanged'] = all(sha(ROOT / p) == digest for p, digest in report['source_files'].items())
        report['success'] &= report['inputs_unchanged']
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    if not report['success']:
        raise RuntimeError('Source changed during focused validation')
    freeze = {'source_files': report['source_files'], 'kernel_dependencies': report['kernel_dependencies'],
              'report_sha256': sha(out / 'report.json'), 'no_target_or_gpu_execution': True}
    (out / 'source-freeze.json').write_text(json.dumps(freeze, indent=2) + '\n')
    print(json.dumps({'report_sha256': sha(out / 'report.json'),
                      'freeze_sha256': sha(out / 'source-freeze.json')}))


if __name__ == '__main__':
    main()
