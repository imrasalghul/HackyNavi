#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate actual-read journals without GPU, IOKit execution or SSH."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OWNED = ['include/hackynavi/' + name + '.hpp' for name in
         ('read_capture', 'boot_display_capture', 'boot_display_stop_capture', 'cold_dma_capture')]
OWNED += ['kext/HackyNavi/HackyNaviReadCapture.hpp', 'scripts/check-read-capture-local.py',
          'docs/actual-read-capture.md']
SUITES = {
    'boot_display': ['src/discovery.cpp', 'src/rom.cpp', 'src/memory_layout.cpp',
                     'src/boot_display.cpp', 'tests/boot_display_tests.cpp'],
    'boot_display_stop': ['src/discovery.cpp', 'src/rom.cpp', 'src/memory_layout.cpp',
                          'src/boot_display.cpp', 'src/boot_display_stop.cpp', 'tests/boot_display_stop_tests.cpp'],
    'cold_dma': ['src/discovery.cpp', 'src/cold_dma.cpp', 'tests/cold_dma_tests.cpp'],
}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    if out.exists():
        parser.error('Use a fresh output directory')
    out.mkdir(parents=True)
    zig = ROOT / '.tools/zig-x86_64-windows-0.17.0/zig.exe'
    files = sorted(set(OWNED + sum(SUITES.values(), [])))
    report = {'success': False, 'gpu_executed': False, 'ssh_used': False, 'target_executed': False,
              'sources': {p: sha(ROOT / p) for p in files}, 'commands': [], 'compiler_sha256': sha(zig)}

    def run(command, negative=False):
        r = subprocess.run([str(v) for v in command], cwd=ROOT, capture_output=True, text=True, timeout=300)
        report['commands'].append({'args': [str(v) for v in command], 'exit_code': r.returncode,
                                   'stdout': r.stdout, 'stderr': r.stderr[:12000], 'negative_control': negative})
        if (r.returncode == 0) == negative:
            raise RuntimeError(r.stdout + r.stderr)

    try:
        common = [zig, 'c++', '-std=c++11', '-Wall', '-Wextra', '-Wpedantic', '-Werror']
        for name, sources in SUITES.items():
            for kind, flags in [('strict', []), ('ubsan', ['-fsanitize=undefined', '-fno-sanitize-recover=all'])]:
                exe = out / (name + '-' + kind + '.exe')
                run([*common, *flags, '-Iinclude', *sources, '-o', exe])
                run([exe])
        # These mutants must fail real sequence/value/mask assertions, not
        # source-text checks. Compile only the smallest full protocol suite.
        text = (ROOT / 'include/hackynavi/read_capture.hpp').read_text()
        mutations = {
            'lost-callback-result': text.replace('return succeeded;', 'return true;'),
            'lost-failed-read-record': text.replace('if (out.recorded < Capacity)', 'if (succeeded && out.recorded < Capacity)'),
        }
        for name, source in mutations.items():
            override = out / name / 'hackynavi'
            override.mkdir(parents=True)
            (override / 'read_capture.hpp').write_text(source)
            exe = out / (name + '.exe')
            run([*common, '-I' + str(override.parent), '-Iinclude', *SUITES['cold_dma'], '-o', exe])
            run([exe], negative=True)
        # Force instantiation of every native off-stack adapter and serializer.
        source = out / 'native_read_capture.cpp'
        source.write_text('''#include "HackyNaviReadCapture.hpp"
#include "hackynavi/boot_display_capture.hpp"
#include "hackynavi/boot_display_stop_capture.hpp"
#include "hackynavi/cold_dma_capture.hpp"
using namespace hackynavi;
bootdisplay::Status display(const bootdisplay::Layout& l,const MemoryLayout& m,
 const bootdisplay::Io& io,bootdisplay::Capture& c,OSDictionary* d) {
 HackyNaviReadCapture<bootdisplay::ReadCapture> t;
 auto s=bootdisplay::captureRecorded(l,m,io,c,t.get(),true);t.publish(d,"Reads");return s;
}
bootdisplaystop::Status owned(const bootdisplay::Layout& l,const MemoryLayout& m,
 const bootdisplaystop::Io& io,bootdisplaystop::State& state,bootdisplaystop::Result& r,OSDictionary* d) {
 HackyNaviReadCapture<bootdisplaystop::ReadCapture> t;
 auto s=bootdisplaystop::captureRecorded(l,m,io,state,r,t.get());t.publish(d,"Reads");return s;
}
colddma::Status dma(const colddma::Layout& l,const colddma::Io& io,colddma::Result& r,OSDictionary* d) {
 HackyNaviReadCapture<colddma::ReadCapture> t;
 auto s=colddma::checkStoppedRecorded(l,io,r,t.get());t.publish(d,"Reads");return s;
}
''')
        kernel = [zig, 'c++', '-target', 'x86_64-macos.15.0', '-std=c++11', '-mkernel', '-fapple-kext',
                  '-DKERNEL', '-DKERNEL_EXTENSION', '-nostdinc', '-nostdlibinc',
                  '-I.research/MacKernelSDK/Headers', '-Iinclude', '-Ikext/HackyNavi',
                  '-fno-exceptions', '-fno-rtti', '-fno-builtin', '-fno-common', '-fno-stack-protector',
                  '-mno-red-zone', '-mno-mmx', '-mno-sse', '-mno-sse2', '-Wall', '-Wextra', '-Werror',
                  '-Wframe-larger-than=2048', '-fstack-usage', '-O2']
        dep = out / 'native_read_capture.d'
        run([*kernel, '-M', '-MT', 'native_read_capture.o', '-MF', dep, source])
        paths = dep.read_text().replace('\\\n', ' ').split(':', 1)[1].split()
        report['kernel_dependencies'] = {p: sha(ROOT / p) for p in paths}
        run([*kernel, '-c', source, '-o', out / 'native_read_capture.o'])
        report['artifacts'] = {p.name: sha(p) for p in out.iterdir() if p.suffix in ('.exe', '.o', '.su')}
        report['success'] = True
    finally:
        report['inputs_unchanged'] = all(sha(ROOT / p) == v for p, v in report['sources'].items())
        report['inputs_unchanged'] &= all(sha(ROOT / p) == v for p, v in report.get('kernel_dependencies', {}).items())
        report['success'] &= report['inputs_unchanged']
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    if not report['success']:
        raise RuntimeError('Read capture validation failed')
    print(json.dumps({'success': True, 'report_sha256': sha(out / 'report.json')}))


if __name__ == '__main__':
    main()
