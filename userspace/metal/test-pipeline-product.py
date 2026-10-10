#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build/test real owned compiler products locally using existing tool caches."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
def main():
    local=ROOT/'.tools/userspace';cargo=local/'cargo/bin/cargo.exe'
    llvm=local/'src/clang+llvm-20.1.8-x86_64-pc-windows-msvc/bin'
    env=os.environ.copy();env['RUSTUP_HOME']=str(local/'rustup');env['CARGO_HOME']=str(local/'cargo')
    gcc=local/'rustup/toolchains/1.90.0-x86_64-pc-windows-gnu/lib/rustlib/x86_64-pc-windows-gnu/bin/self-contained'
    env['PATH']=str(cargo.parent)+os.pathsep+str(gcc)+os.pathsep+env.get('PATH','')
    env['CARGO_TARGET_DIR']=str(ROOT/'build/metal-compiler')
    env['METAL2VULKAN_LLVM_DIS']=str(llvm/'llvm-dis.exe')
    env['METAL2VULKAN_SPIRV_VAL']=str(local/'build-spirv-tools/tools/spirv-val.exe')
    env['CARGO_ENCODED_RUSTFLAGS']='\x1f'.join(['-C',f'dlltool={llvm/"llvm-dlltool.exe"}'])
    compiler=local/'build-mesa-compiler/hackynavi-compiler/hackynavi-aco.exe'
    output=ROOT/'build/metal-pipeline-product';output.mkdir(parents=True,exist_ok=True)
    sources=[*HERE.joinpath('src').glob('*.rs'),*HERE.joinpath('include').glob('*.h'),HERE/'Cargo.toml',HERE/'Cargo.lock',
        HERE/'tests/check_pipeline_c_abi.py',HERE/'tests/fixtures/affine_xor.ll',HERE/'tests/fixtures/apple/add_one.air',
        HERE/'test-pipeline-product.py',ROOT/'userspace/compiler/compiler.c',compiler,
        Path(env['METAL2VULKAN_SPIRV_VAL']),HERE/'vendor/source-manifest.json']
    hashes={str(p.relative_to(ROOT)).replace('\\','/'):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
    commands=[[str(cargo),'test','--locked','--offline','--manifest-path',str(HERE/'Cargo.toml'),'--lib','--','--nocapture'],
        [str(cargo),'build','--locked','--offline','--manifest-path',str(HERE/'Cargo.toml'),'--lib'],
        [sys.executable,str(HERE/'tests/check_pipeline_c_abi.py'),str(ROOT/'build/metal-compiler/debug/hackynavi_metal_compiler.dll'),str(compiler)]]
    report={'scope':'owned offline ACO pipeline products','hardware_executed':False,'metal_device_registered':False,
        'sources':hashes,'commands':[]};log=[]
    for command in commands:
        result=subprocess.run(command,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        print(result.stdout,end='',flush=True);log.append(result.stdout)
        report['commands'].append({'argv':command,'exit_code':result.returncode})
        if result.returncode:break
    report['success']=len(report['commands'])==len(commands) and all(c['exit_code']==0 for c in report['commands'])
    report['snapshot_matches_worktree']=all(hashlib.sha256((ROOT/p).read_bytes()).hexdigest()==h for p,h in hashes.items())
    (output/'test.log').write_text(''.join(log),encoding='utf-8')
    (output/'report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    return 0 if report['success'] and report['snapshot_matches_worktree'] else 1
if __name__=='__main__':raise SystemExit(main())
