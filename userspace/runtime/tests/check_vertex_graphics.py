#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Actual immutable vertex products and additive wire contracts; no GPU/mocks."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[3]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    out=parser.parse_args().output.resolve()
    if out.exists():parser.error('Fresh evidence output required')
    evidence=['build/gfx11-vertex-buffer-tests-1/report.json','build/apple-graphics-integration-1/report.json',
              'build/gfx11-fragment-buffer-tests-4/report.json']
    vertex,legacy,fragment=[json.loads((ROOT/name).read_text()) for name in evidence]
    if not all(row['success'] for row in (vertex,legacy,fragment)):parser.error('Actual compiler evidence failed')
    fixtures=[('build/gfx11-vertex-buffer-tests-1/air-cli/shader.bin',vertex['products'][0]['code_sha256']),
              ('build/apple-graphics-integration-1/air-fragment/product/shader.bin',legacy['products'][1]['code_sha256']),
              ('build/gfx11-fragment-buffer-tests-4/air/shader.bin',fragment['products'][0]['code_sha256']),
              ('build/apple-graphics-integration-1/air-vertex/product/shader.bin',legacy['products'][0]['code_sha256'])]
    for name,digest in fixtures:
        if sha(ROOT/name)!=digest:parser.error('Actual fixture changed: '+name)
    units=['userspace/runtime/tests/vertex_graphics_tests.cpp','userspace/runtime/graphics_buffer_snapshot.cpp',
           'userspace/runtime/graphics_snapshot.cpp','userspace/transport/native_protocol.cpp',
           'src/gfx11_graphics_product.cpp','src/gfx11_color_target.cpp']
    sources=units+['userspace/runtime/tests/check_vertex_graphics.py','tests/native_abi_tests.cpp',
        'userspace/runtime/graphics_buffer_snapshot.hpp','userspace/runtime/graphics_snapshot.hpp',
        'userspace/transport/iokit_client.hpp','include/hackynavi/native_abi.hpp',
        'include/hackynavi/gfx11_graphics_product.hpp','include/hackynavi/gfx11_color_target.h']
    zig=ROOT/'.tools/zig-x86_64-windows-0.17.0/zig.exe'
    report={'success':False,'sources':{n:sha(ROOT/n) for n in sources},'fixtures':dict(fixtures),
        'compiler_sha256':sha(zig),'evidence':{n:sha(ROOT/n) for n in evidence},'commands':[],
        'gpu_executed':False,'context_opened':False,'mock_backend':False}
    out.mkdir(parents=True)
    def run(argv):
        process=subprocess.run([str(x) for x in argv],cwd=ROOT,capture_output=True,text=True,timeout=120)
        report['commands'].append({'args':[str(x) for x in argv],'exit_code':process.returncode,
            'stdout':process.stdout,'stderr':process.stderr})
        if process.returncode:raise RuntimeError(process.stdout+process.stderr)
    try:
        flags=[zig,'c++','-std=c++17','-Wall','-Wextra','-Wpedantic','-Werror','-Iinclude']
        for label,extra in [('strict',['-O2']),('ubsan',['-O1','-g','-fsanitize=undefined','-fno-sanitize-recover=all'])]:
            run([*flags,*extra,*units,'-o',out/(label+'.exe')])
            run([out/(label+'.exe'),*[ROOT/name for name,_ in fixtures]])
            run([*flags,*extra,'tests/native_abi_tests.cpp','userspace/transport/native_protocol.cpp',
                 '-o',out/(label+'-legacy-abi.exe')])
            run([out/(label+'-legacy-abi.exe')])
        report['artifacts']={p.name:sha(p) for p in out.iterdir() if p.suffix=='.exe'}
        report['success']=True
    finally:
        report['inputs_unchanged']=all(sha(ROOT/n)==digest for n,digest in report['sources'].items())
        report['success'] &= report['inputs_unchanged']
        (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'success':report['success'],'report_sha256':sha(out/'report.json')}))


if __name__=='__main__':main()
