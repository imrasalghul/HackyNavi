#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check real Apple vertex-buffer translation and owned runtime reflection. CPU only."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT=Path(__file__).resolve().parents[3]
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path)
    out=p.parse_args().output.resolve()
    if out.exists():p.error('Use a new evidence directory')
    prior=ROOT/'build/apple-vertex-buffer-translation-1/report.json'
    tr=json.loads(prior.read_text());assert tr['success'] and tr['air_metallib_identical']
    tools=tr['tools']
    for name in ('compiler','air_dis','spirv_val'):
        assert sha(Path(tools[name]['path']))==tools[name]['sha256']
    fixtures=[('userspace/compiler/fixtures/apple-vertex-buffer/vertex-buffer.air',tr['products'][0]['input_sha256']),
              ('userspace/compiler/fixtures/apple-vertex-buffer/vertex-buffer.metallib',tr['products'][1]['input_sha256']),
              ('build/apple-vertex-buffer-translation-1/air/shader.spv',tr['products'][0]['spirv_sha256'])]
    for name,digest in fixtures:assert sha(ROOT/name)==digest
    # Recursively bind the actual project headers this translation unit uses.
    pending=['userspace/runtime/graphics_shader.cpp','userspace/runtime/tests/graphics_vertex_shader_tests.cpp',
             'userspace/runtime/tests/check_graphics_vertex_shader.py'];sources={}
    while pending:
        name=pending.pop()
        if name in sources:continue
        path=ROOT/name;sources[name]=sha(path)
        if path.suffix=='.py':continue
        for delimiter,inc in re.findall(r'^\s*#include\s+(["<])([^">]+)[">]',path.read_text(),re.M):
            if delimiter=='<' and not inc.startswith('hackynavi/'):continue
            found=next((x for x in (path.parent/inc,ROOT/'include'/inc) if x.is_file()),None)
            if found is None:raise RuntimeError('Unresolved project include: '+inc)
            pending.append(found.resolve().relative_to(ROOT).as_posix())
    zig=ROOT/'.tools/zig-x86_64-windows-0.17.0/zig.exe'
    report={'success':False,'source_files':sources,'fixtures':dict(fixtures),'translation_report_sha256':sha(prior),
            'tools':tools,'compiler_sha256':sha(zig),'commands':[],'gpu_executed':False,'remote_executed':False}
    out.mkdir(parents=True);env=os.environ.copy()
    env['PATH']=str(Path(tools['compiler']['path']).parent)+os.pathsep+env.get('PATH','')
    def run(argv):
        r=subprocess.run([str(x) for x in argv],cwd=ROOT,env=env,capture_output=True,text=True,timeout=120)
        report['commands'].append({'argv':[str(x) for x in argv],'exit_code':r.returncode,'stdout':r.stdout,'stderr':r.stderr})
        if r.returncode:raise RuntimeError(r.stdout+r.stderr)
    try:
        flags=[zig,'c++','-std=c++17','-Wall','-Wextra','-Wpedantic','-Werror','-Iinclude']
        for label,extra in [('strict',['-O2']),('ubsan',['-O1','-g','-fsanitize=undefined','-fno-sanitize-recover=all'])]:
            exe=out/(label+'.exe')
            run([*flags,*extra,'userspace/runtime/graphics_shader.cpp','userspace/runtime/tests/graphics_vertex_shader_tests.cpp',tools['compiler']['path'],'-o',exe])
            run([exe,*[ROOT/name for name,_ in fixtures],tools['air_dis']['path'],tools['spirv_val']['path']])
        run([*flags,'-target','x86_64-macos.15.0','-O2','-c','userspace/runtime/graphics_shader.cpp','-o',out/'graphics_shader.o'])
        report['artifacts']={x.name:sha(x) for x in out.iterdir() if x.suffix in ('.exe','.o')}
        report['success']=True
    finally:
        report['inputs_unchanged']=all(sha(ROOT/name)==digest for name,digest in sources.items())
        report['success'] &= report['inputs_unchanged']
        (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'success':report['success'],'report_sha256':sha(out/'report.json')}))
    if not report['success']:raise RuntimeError('Source changed during validation')
if __name__=='__main__':main()
