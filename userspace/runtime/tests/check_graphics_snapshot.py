#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Local actual-product snapshot/bounds tests; no transport or GPU execution."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[3]
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args();out=args.output.resolve()
    if out.exists():parser.error('Use a new output directory; previous evidence is immutable')
    names=['userspace/runtime/compute_context.hpp','userspace/runtime/compute_context.cpp',
        'userspace/runtime/graphics_snapshot.hpp','userspace/runtime/graphics_snapshot.cpp',
        'userspace/runtime/tests/graphics_snapshot_tests.cpp','userspace/runtime/tests/check_graphics_snapshot.py',
        'userspace/runtime/GRAPHICS.md','userspace/transport/iokit_client.hpp',
        'include/hackynavi/native_abi.hpp','include/hackynavi/gfx11_graphics_product.hpp',
        'include/hackynavi/gfx11_color_target.h','src/gfx11_graphics_product.cpp','src/gfx11_color_target.cpp',
        'userspace/metal/include/hackynavi_shader.h','userspace/metal/include/hackynavi_dispatch.h',
        'userspace/metal/include/hackynavi_pipeline.h']
    # Resolve paths from the reviewed actual-product evidence, then independently
    # verify their fixed digests before allowing them into this new test run.
    evidence=json.loads((ROOT/'build/gfx11-graphics-draw-local-4/report.json').read_text())
    expected=['ffa5d5142a6f8a5786296ba27f92e631c236bfd35161279b28919dd8997c9c61',
        '15f6f976c7b77857915d8254f55d028c8413bcf74b96f8403767f1d90631f36c',
        '69619a05dddf65ae1d50c406cf0a395b32c28e92e02eafa2122a29c99ef02b05',
        '340aea59bb687732a3e16305fd6a61b169838114630137bdfc883f6054b0d9e8',
        'c969de47af807b6c89ce36abb60514fc3b81bb4ef54ed36ebb4246d73eded247',
        'be6733d4607dd5517aa33c0403e07bd032025ae3a3b16f1819c55716de45bd91']
    fixtures=[]
    for digest in expected:
        matches=[ROOT/name for name,value in evidence['files'].items() if value==digest and name.endswith('shader.bin')]
        if len(matches)!=1 or sha(matches[0])!=digest:raise RuntimeError('Immutable actual compiler product missing/changed')
        fixtures.append(matches[0])
    zig=ROOT/'.tools/zig-x86_64-windows-0.17.0/zig.exe'
    report={'success':False,'files':{n:sha(ROOT/n) for n in names},
        'fixtures':{p.relative_to(ROOT).as_posix():sha(p) for p in fixtures},
        'fixture_evidence_sha256':sha(ROOT/'build/gfx11-graphics-draw-local-4/report.json'),
        'compiler_sha256':sha(zig),'commands':[],'gpu_executed':False,'native_client_executed':False}
    out.mkdir(parents=True)
    def run(argv):
        result=subprocess.run([str(x) for x in argv],cwd=ROOT,text=True,capture_output=True,timeout=120)
        report['commands'].append({'argv':[str(x) for x in argv],'exit_code':result.returncode,
            'stdout':result.stdout,'stderr':result.stderr})
        if result.returncode:raise RuntimeError(result.stdout+result.stderr)
    try:
        flags=[zig,'c++','-std=c++17','-Wall','-Wextra','-Wpedantic','-Werror','-Iinclude']
        sources=['userspace/runtime/graphics_snapshot.cpp','src/gfx11_graphics_product.cpp',
            'src/gfx11_color_target.cpp','userspace/runtime/tests/graphics_snapshot_tests.cpp']
        for label,extra in [('strict',['-O2']),('ubsan',['-O1','-g','-fsanitize=undefined','-fno-sanitize-recover=all'])]:
            run([*flags,*extra,*sources,'-o',out/(label+'.exe')]);run([out/(label+'.exe'),*fixtures])
        for source in ['userspace/runtime/compute_context.cpp','userspace/runtime/graphics_snapshot.cpp']:
            run([*flags,'-target','x86_64-macos.15.0','-O2','-c',source,'-o',out/(Path(source).stem+'.o')])
        report['artifacts']={p.name:sha(p) for p in out.iterdir() if p.suffix in ('.exe','.o')}
        report['success']=True
    finally:
        report['inputs_unchanged']=all(sha(ROOT/n)==value for n,value in report['files'].items())
        report['success'] &= report['inputs_unchanged']
        (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'report':str(out/'report.json'),'sha256':sha(out/'report.json'),'success':report['success']}))
    if not report['success']:raise RuntimeError('Graphics snapshot validation failed')
if __name__=='__main__':main()
