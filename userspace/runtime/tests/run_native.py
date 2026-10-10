#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verified-stage native runtime compilation and real compiler-backed offline tests."""
import hashlib,json,os,re,signal,subprocess,sys,tarfile,time
from pathlib import Path

def main():
    stage=Path(sys.argv[1]).resolve()
    if not re.fullmatch(r'/Users/[^/\r\n]+/Library/Caches/HackyNavi-ComputeRuntime\.[A-Za-z0-9]+',str(stage)):
        raise RuntimeError('Unexpected runtime stage')
    if Path(__file__).resolve()!=stage/'userspace/runtime/tests/run_native.py':
        raise RuntimeError('Runner not inside exact stage')
    os.chdir(stage)
    report={'stage':str(stage),'complete':False,'success':False,'child_processes_reaped':True,
            'gpu_executed':False,'context_open_called':False,'smoke_executed':False,'commands':[]}
    prior_path=stage/'provenance/prior-runtime-report.json'
    prior=json.loads(prior_path.read_text()) if prior_path.exists() else None
    extension_path=stage/'provenance/runtime-extension.json'
    mode=json.loads(extension_path.read_text()) if extension_path.exists() else None
    if mode is not None and (not prior or mode not in ({'mode':'shader-library'},{'mode':'compute-pipeline'},{'mode':'command-objects'})):
        raise RuntimeError('Invalid runtime extension mode')
    extend_shader=mode=={'mode':'shader-library'}
    extend_pipeline=mode=={'mode':'compute-pipeline'}
    extend_command=mode=={'mode':'command-objects'}
    extension=extend_shader or extend_pipeline or extend_command
    pipeline_layer=extend_pipeline or extend_command
    report['relink_only']=bool(prior) and not extension
    report['extend_shader']=extend_shader;report['extend_pipeline']=extend_pipeline;report['extend_command']=extend_command;report['offline_tests_rerun']=not bool(prior)
    env=os.environ.copy();(stage/'tmp').mkdir()
    env.update(TMPDIR=str(stage/'tmp'),METAL2VULKAN_LLVM_DIS=str(stage/'apple-air-dis'),
               METAL2VULKAN_SPIRV_VAL=str(stage/'spirv-val'),ASAN_OPTIONS='detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1')
    def persist():
        (stage/'native-report.json').write_text(json.dumps(report,indent=2)+'\n')
    def alive(pid):
        try:os.killpg(pid,0);return True
        except ProcessLookupError:return False
    def reap(process):
        for sig in (signal.SIGTERM,signal.SIGKILL):
            try:os.killpg(process.pid,sig)
            except ProcessLookupError:pass
            try:process.wait(timeout=2)
            except subprocess.TimeoutExpired:pass
            deadline=time.monotonic()+2
            while alive(process.pid) and time.monotonic()<deadline:time.sleep(.05)
            if not alive(process.pid):return True
        return False
    def run(argv):
        item={'argv':argv,'exit_code':None};report['commands'].append(item);persist()
        process=subprocess.Popen(argv,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
        try:
            stdout,stderr=process.communicate(timeout=180)
            if alive(process.pid) and not reap(process):
                report['child_processes_reaped']=False
                raise RuntimeError('Remaining descendant process group; preserve stage')
            item.update(exit_code=process.returncode,stdout=stdout,stderr=stderr)
        except BaseException:
            if not reap(process):report['child_processes_reaped']=False
            item['error']='Command interrupted; group termination attempted';persist();raise
        used=sum(p.stat().st_size for p in stage.rglob('*') if p.is_file())
        report['max_observed_stage_bytes']=max(used,report.get('max_observed_stage_bytes',0));persist()
        if used>200*1024*1024:raise RuntimeError('Stage exceeds200MiB observed budget')
        if process.returncode:raise RuntimeError('Native command failed: '+argv[0])
        return item
    try:
        run(['sw_vers']);run(['xcrun','clang++','--version'])
        report['sdk_version']=run(['xcrun','--sdk','macosx','--show-sdk-version'])['stdout'].strip()
        sdk=Path(run(['xcrun','--sdk','macosx','--show-sdk-path'])['stdout'].strip()).resolve()
        metal_headers=sdk/'System/Library/Frameworks/Metal.framework/Headers'
        report['public_metal_headers']={path.name:{'bytes':path.stat().st_size,
            'sha256':hashlib.sha256(path.read_bytes()).hexdigest()} for path in sorted(metal_headers.glob('*.h'))}
        if not {'MTLBuffer.h','MTLResource.h','MTLDevice.h','MTLAllocation.h'}<=report['public_metal_headers'].keys():
            raise RuntimeError('Required actual public Metal headers unavailable')
        allocation=(metal_headers/'MTLAllocation.h').read_bytes()
        if len(allocation)>65536:raise RuntimeError('MTLAllocation header exceeds requested bound')
        report['public_metal_header_text']={'MTLAllocation.h':allocation.decode('utf-8')}
        (stage/'MTLAllocation.h').write_bytes(allocation)
        if prior:
            if report['sdk_version']!=prior['native']['sdk_version'] or report['public_metal_headers']!=prior['native']['public_metal_headers']:
                raise RuntimeError('SDK changed since prior tested runtime archive')
            report['prior_offline_evidence']={'report_sha256':hashlib.sha256(prior_path.read_bytes()).hexdigest(),
                'checks':prior['native'].get('offline_tests',prior['native'].get('prior_offline_evidence',{})).get('checks'),
                'executed_this_run':False}
            if extend_command:
                report['prior_admission_evidence']={'report_sha256':hashlib.sha256(prior_path.read_bytes()).hexdigest(),
                    'checks':prior['native']['admission_tests']['checks'],'executed_this_run':False}
        persist()
        common=['xcrun','clang++','-std=c++17','-arch','x86_64','-mmacosx-version-min=15.0',
                '-Wall','-Wextra','-Wpedantic','-Werror','-Iinclude']
        sources=['userspace/runtime/pipeline_snapshot.cpp','userspace/runtime/compute_context.cpp',
                 'userspace/transport/native_protocol.cpp','userspace/transport/iokit_client.cpp']
        objects=[]
        if prior:
            objects=[Path(source).stem+'.o' for source in sources]
            members=run(['xcrun','ar','-t','prior-runtime.a'])['stdout'].splitlines()
            ordinary=[name for name in members if name not in {'__.SYMDEF','__.SYMDEF SORTED'}]
            expected_members=objects+['HNMetalBuffer.o']+(['compute_shader.o','HNMetalLibrary.o'] if pipeline_layer else [])+(['HNMetalComputePipeline.o','gfx11_compute_product.o'] if extend_command else [])
            if sorted(ordinary)!=sorted(expected_members):
                raise RuntimeError('Prior runtime archive member list mismatch')
            if extension:objects.append('HNMetalBuffer.o')
            if pipeline_layer:objects+=['compute_shader.o','HNMetalLibrary.o']
            if extend_command:objects+=['HNMetalComputePipeline.o','gfx11_compute_product.o']
            run(['xcrun','ar','-x','prior-runtime.a',*objects])
            report['reused_objects']={name:{'bytes':(stage/name).stat().st_size,
                'sha256':hashlib.sha256((stage/name).read_bytes()).hexdigest()} for name in objects}
        else:
            for source in sources:
                obj=Path(source).stem+'.o';objects.append(obj)
                run(common+['-O2','-c',source,'-o',obj])
        objc='userspace/metal/native/HNMetalBuffer.mm'
        core_common=[flag if flag!='-std=c++17' else '-std=c++14' for flag in common]
        if extend_shader:
            run(common+['-O2','-c','userspace/runtime/compute_shader.cpp','-o','compute_shader.o'])
            run(common+['-O2','-fobjc-arc','-c','userspace/metal/native/HNMetalLibrary.mm','-o','HNMetalLibrary.o'])
            objects+=['compute_shader.o','HNMetalLibrary.o']
        elif extend_pipeline:
            run(common+['-O2','-fobjc-arc','-c','userspace/metal/native/HNMetalComputePipeline.mm','-o','HNMetalComputePipeline.o'])
            # Match the real kernel's C++14 requirement for this exact shared TU.
            run(core_common+['-O2','-c','src/gfx11_compute_product.cpp','-o','gfx11_compute_product.o'])
            objects+=['HNMetalComputePipeline.o','gfx11_compute_product.o']
        elif extend_command:
            run(core_common+['-O2','-c','userspace/runtime/command_lifecycle.cpp','-o','command_lifecycle.o'])
            run(common+['-O2','-fobjc-arc','-c','userspace/metal/native/HNMetalCommand.mm','-o','HNMetalCommand.o'])
            objects+=['command_lifecycle.o','HNMetalCommand.o']
        else:
            run(common+['-O2','-fobjc-arc','-c',objc,'-o','HNMetalBuffer.o']);objects.append('HNMetalBuffer.o')
        archive='libHackyNaviComputeRuntime.a';library='libHackyNaviComputeRuntime.dylib'
        run(['xcrun','libtool','-static','-o',archive,*objects])
        link=['-L.','-lhackynavi_metal_compiler','-framework','IOKit','-framework','CoreFoundation','-framework','Foundation','-framework','Metal',
              '-Wl,-rpath,@loader_path']
        run(common+['-O2','-dynamiclib','-Wl,-force_load,'+str(stage/archive),
                    '-Wl,-install_name,@rpath/'+library,*link,'-o',library])
        test='pipeline-snapshot-tests'
        if not prior:
            # Compile every actual production TU into the sanitized offline binary.
            # No hardware Context method is invoked by pipeline_snapshot_tests.cpp.
            run(common+['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                        '-fobjc-arc','-c',objc,'-o','HNMetalBuffer-sanitized.o'])
            run(common+['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                        *sources,'HNMetalBuffer-sanitized.o','userspace/runtime/tests/pipeline_snapshot_tests.cpp',*link,'-o',test])
            item=run([str(stage/test),str(stage/'hackynavi-aco'),
                      str(stage/'userspace/metal/tests/fixtures/apple/add_one.air'),
                      str(stage/'userspace/metal/tests/fixtures/affine_xor.ll'),
                      str(stage/'userspace/runtime/tests/fixtures/affine_xor_reordered.ll')])
            results=json.loads(item['stdout'])
            if not results['success'] or results['hardware_executed'] or results['context_open_called']:
                raise RuntimeError('Unexpected offline test result')
            for case in results['cases']:
                for variant in case['variants']:
                    code=bytes.fromhex(variant.pop('code_hex'))
                    if len(code)!=variant['code_bytes']:raise RuntimeError('Reported code length mismatch')
                    variant['code_sha256']=hashlib.sha256(code).hexdigest()
            report['offline_tests']=results
            (stage/'offline-tests.json').write_text(json.dumps(results,indent=2)+'\n')
        if extend_shader:
            # Only the new Shader API's offline compiler/ownership tests run.
            # The five reused production objects remain byte-authenticated;
            # neither Metal class nor Context is instantiated by this program.
            test='compute-shader-tests'
            run(common+['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                'userspace/runtime/compute_shader.cpp','userspace/runtime/pipeline_snapshot.cpp',
                'userspace/runtime/tests/compute_shader_tests.cpp',*link,'-o',test])
            item=run([str(stage/test),str(stage/'hackynavi-aco'),
                str(stage/'userspace/metal/tests/fixtures/apple/add_one.air'),
                str(stage/'userspace/metal/tests/fixtures/affine_xor.ll'),
                str(stage/'userspace/metal/tests/fixtures/function_constant_partial.ll')])
            results=json.loads(item['stdout'])
            if not results['success'] or results['hardware_executed'] or results['context_open_called']:
                raise RuntimeError('Unexpected Shader offline test result')
            for case in results['cases']:
                for variant in case['variants']:
                    code=bytes.fromhex(variant.pop('code_hex'))
                    if len(code)!=variant['code_bytes']:raise RuntimeError('Reported Shader code length mismatch')
                    variant['code_sha256']=hashlib.sha256(code).hexdigest()
            report['shader_tests']=results
            (stage/'shader-tests.json').write_text(json.dumps(results,indent=2)+'\n')
        if extend_pipeline:
            # New boundary only: actual compiler products are validated by the
            # same source that native publication calls. No ObjC object or
            # hardware Context is constructed and prior252 checks are not rerun.
            test='compute-pipeline-admission-tests'
            run(core_common+['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                '-c','src/gfx11_compute_product.cpp','-o','gfx11_compute_product-sanitized.o'])
            run(common+['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                'userspace/runtime/compute_shader.cpp','userspace/runtime/pipeline_snapshot.cpp',
                'gfx11_compute_product-sanitized.o','userspace/runtime/tests/compute_pipeline_admission_tests.cpp',*link,'-o',test])
            item=run([str(stage/test),str(stage/'hackynavi-aco'),
                str(stage/'userspace/metal/tests/fixtures/apple/add_one.air'),
                str(stage/'userspace/metal/tests/fixtures/affine_xor.ll'),
                str(stage/'userspace/metal/tests/fixtures/metallib/apple-32023-macos15-add-one.air'),
                str(stage/'userspace/metal/tests/fixtures/metallib/apple-32023-macos15-add-one.metallib')])
            results=json.loads(item['stdout'])
            if not results['success'] or results['hardware_executed'] or results['context_open_called'] or results['metal_object_created']:
                raise RuntimeError('Unexpected pipeline admission offline result')
            for case in results['cases']:
                for variant in case['variants']:
                    code=bytes.fromhex(variant.pop('code_hex'))
                    if len(code)!=variant['code_bytes'] or variant['admission']!='Ok':
                        raise RuntimeError('Reported admission code/result mismatch')
                    variant['code_sha256']=hashlib.sha256(code).hexdigest()
            report['admission_tests']=results
            (stage/'admission-tests.json').write_text(json.dumps(results,indent=2)+'\n')
        if extend_command:
            # Host lifecycle/order bookkeeping only. This actual production
            # module contains no GPU backend or executor. No prior compiler,
            # Pipeline or ISA suite is repeated by this focused extension.
            test='command-lifecycle-tests'
            run(core_common+['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                'userspace/runtime/command_lifecycle.cpp','userspace/runtime/tests/command_lifecycle_tests.cpp','-o',test])
            results=json.loads(run([str(stage/test)])['stdout'])
            if not results['success'] or not results['host_lifecycle_only'] or results['hardware_executed']:
                raise RuntimeError('Unexpected host lifecycle test result')
            report['command_tests']=results
            (stage/'command-tests.json').write_text(json.dumps(results,indent=2)+'\n')
        # This executable is only built. It may open the target GPU when the
        # user explicitly runs it later; never invoke it in this runner.
        smoke='hackynavi-context-smoke'
        run(common+['-O2','userspace/runtime/context-smoke.cpp','-L.','-lHackyNaviComputeRuntime',*link,'-o',smoke])
        run(['file',archive,library,smoke]+([test] if not prior or extension else []));run(['otool','-L',library]);run(['otool','-L',smoke])
        symbols=run(['nm','-gU',library])['stdout']
        if 'Context4open' not in symbols or 'Pipeline7capture' not in symbols or '_OBJC_CLASS_$_HNMetalBuffer' not in symbols:
            raise RuntimeError('Actual runtime entrypoints missing from linked image')
        if extension and not all(name in symbols for name in ['Shader7compile','_OBJC_CLASS_$_HNMetalLibrary','_OBJC_CLASS_$_HNMetalFunction']):
            raise RuntimeError('Actual Shader/Metal library entrypoints missing from linked image')
        if pipeline_layer and not all(name in symbols for name in ['_OBJC_CLASS_$_HNMetalComputePipeline','gfx11product8validate']):
            raise RuntimeError('Actual compute pipeline/shared admission entrypoints missing from linked image')
        if extend_command and not all(name in symbols for name in ['_OBJC_CLASS_$_HNMetalCommandQueue',
            '_OBJC_CLASS_$_HNMetalCommandBuffer','_OBJC_CLASS_$_HNMetalCommandEncoder','command9Scheduler']):
            raise RuntimeError('Actual command object/host scheduler entrypoints missing from linked image')
        exported=[archive,library,smoke,'MTLAllocation.h']+([] if prior else [test,'offline-tests.json'])
        if extend_shader:exported += [test,'shader-tests.json']
        if extend_pipeline:exported += [test,'admission-tests.json']
        if extend_command:exported += [test,'command-tests.json']
        report['artifacts']={name:{'bytes':(stage/name).stat().st_size,
                            'sha256':hashlib.sha256((stage/name).read_bytes()).hexdigest()} for name in exported}
        with tarfile.open(stage/'artifacts.tar.gz','w:gz') as tar:
            for name in exported:tar.add(stage/name,arcname=name,recursive=False)
        report['artifact_archive_sha256']=hashlib.sha256((stage/'artifacts.tar.gz').read_bytes()).hexdigest()
        report['success']=True
    except BaseException as error:
        report['error']=str(error)
    finally:
        report['complete']=True;persist()
    return 0 if report['success'] else 1

if __name__=='__main__':raise SystemExit(main())
