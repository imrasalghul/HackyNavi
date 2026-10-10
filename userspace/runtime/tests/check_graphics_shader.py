#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Actual explicit compiler ownership and resource-free graphics reflection tests."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[3]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True)
    out=parser.parse_args().output.resolve()
    if out.exists():parser.error('Use a fresh evidence directory')
    evidence=ROOT/'build/apple-graphics-integration-1/report.json'
    prior=json.loads(evidence.read_text());tools=prior['tools']
    for name in ('compiler','air_dis','spirv_val'):
        if sha(Path(tools[name]['path']))!=tools[name]['sha256']:raise RuntimeError('Reviewed compiler/tool changed')
    fixtures=['userspace/compiler/fixtures/apple-graphics/graphics.air',
        'userspace/compiler/fixtures/apple-graphics/graphics.metallib',
        'build/apple-graphics-integration-1/air-vertex/shader.spv',
        'build/apple-graphics-integration-1/air-fragment/shader.spv',
        'userspace/metal/tests/fixtures/metallib/sdl-2.30.11-macos.metallib']
    for name,row in zip(fixtures[:4], [prior['products'][0],prior['products'][2],prior['products'][0],prior['products'][1]]):
        expected=row['input_sha256'] if name.endswith(('.air','.metallib')) else row['spirv_sha256']
        if sha(ROOT/name)!=expected:raise RuntimeError('Frozen actual fixture changed: '+name)
    sources=['userspace/runtime/graphics_shader.hpp','userspace/runtime/graphics_shader.cpp',
        'userspace/runtime/tests/graphics_shader_tests.cpp','userspace/runtime/tests/check_graphics_shader.py',
        'userspace/metal/native/HNMetalLibrary.hpp','userspace/metal/native/HNMetalLibrary.mm',
        'userspace/runtime/compute_shader.hpp','userspace/runtime/compute_context.hpp',
        'userspace/runtime/graphics_snapshot.hpp','userspace/metal/include/hackynavi_shader.h']
    zig=ROOT/'.tools/zig-x86_64-windows-0.17.0/zig.exe'
    report={'success':False,'files':{n:sha(ROOT/n) for n in sources},'fixtures':{n:sha(ROOT/n) for n in fixtures},
        'prior_evidence_sha256':sha(evidence),'tools':tools,'compiler_sha256':sha(zig),'commands':[],
        'gpu_executed':False,'metal_objects_created':False,'objc_compilation':'pending native integration'}
    out.mkdir(parents=True)
    env=os.environ.copy();env['PATH']=str(Path(tools['compiler']['path']).parent)+os.pathsep+env.get('PATH','')
    def run(args):
        p=subprocess.run([str(a) for a in args],cwd=ROOT,env=env,text=True,capture_output=True,timeout=120)
        report['commands'].append({'argv':[str(a) for a in args],'exit_code':p.returncode,'stdout':p.stdout,'stderr':p.stderr})
        if p.returncode:raise RuntimeError(p.stdout+p.stderr)
    try:
        base=[zig,'c++','-std=c++17','-Wall','-Wextra','-Wpedantic','-Werror','-Iinclude']
        for label,flags in [('strict',['-O2']),('ubsan',['-O1','-g','-fsanitize=undefined','-fno-sanitize-recover=all'])]:
            exe=out/(label+'.exe')
            run([*base,*flags,'userspace/runtime/graphics_shader.cpp','userspace/runtime/tests/graphics_shader_tests.cpp',tools['compiler']['path'],'-o',exe])
            run([exe,*[ROOT/n for n in fixtures[:4]],tools['air_dis']['path'],tools['spirv_val']['path'],ROOT/fixtures[4]])
        run([*base,'-target','x86_64-macos.15.0','-O2','-c','userspace/runtime/graphics_shader.cpp','-o',out/'graphics_shader.o'])
        report['success']=True
        report['artifacts']={p.name:sha(p) for p in out.iterdir() if p.suffix in ('.exe','.o')}
    finally:
        report['inputs_unchanged']=all(sha(ROOT/n)==value for n,value in report['files'].items())
        report['success'] &= report['inputs_unchanged']
        (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'report':str(out/'report.json'),'sha256':sha(out/'report.json'),'success':report['success']}))
if __name__=='__main__':main()
