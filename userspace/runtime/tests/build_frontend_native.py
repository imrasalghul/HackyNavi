#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile/link a frozen frontend on the build Mac; never instantiate Metal."""
import hashlib,json,os,re,signal,subprocess,sys,tarfile,time
from pathlib import Path

def main():
    stage=Path(sys.argv[1]).resolve()
    if not re.fullmatch(r'/Users/[^/\r\n]+/Library/Caches/HackyNavi-Frontend\.[A-Za-z0-9]+',str(stage)):
        raise RuntimeError('Unexpected frontend stage')
    if Path(__file__).resolve()!=stage/'userspace/runtime/tests/build_frontend_native.py':
        raise RuntimeError('Runner is outside its exact stage')
    os.chdir(stage)
    plan=json.loads((stage/'build-plan.json').read_text())
    report={'stage':str(stage),'complete':False,'success':False,'child_processes_reaped':True,
            'gpu_executed':False,'metal_instantiated':False,'context_open_called':False,
            'tests_executed':False,'commands':[]}
    report['device_layer']=plan['device_layer']
    env=os.environ.copy();(stage/'tmp').mkdir();env['TMPDIR']=str(stage/'tmp')
    def persist(): (stage/'native-report.json').write_text(json.dumps(report,indent=2)+'\n')
    def measure():
        used=sum(p.stat().st_size for p in stage.rglob('*') if p.is_file())
        report['max_observed_stage_bytes']=max(used,report.get('max_observed_stage_bytes',0));persist()
        if used>128*1024*1024: raise RuntimeError('Stage exceeded 128 MiB')
    def alive(pid):
        try: os.killpg(pid,0);return True
        except ProcessLookupError: return False
    def reap(process):
        for sig in (signal.SIGTERM,signal.SIGKILL):
            try: os.killpg(process.pid,sig)
            except ProcessLookupError: pass
            try: process.wait(timeout=2)
            except subprocess.TimeoutExpired: pass
            deadline=time.monotonic()+2
            while alive(process.pid) and time.monotonic()<deadline: time.sleep(.05)
            if not alive(process.pid): return True
        return False
    def run(argv):
        item={'argv':argv};report['commands'].append(item);persist()
        process=subprocess.Popen(argv,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
        try:
            out,err=process.communicate(timeout=120)
            if alive(process.pid) and not reap(process):
                report['child_processes_reaped']=False
                raise RuntimeError('Unreaped process group')
            item.update(exit_code=process.returncode,stdout=out,stderr=err)
        except BaseException:
            if not reap(process): report['child_processes_reaped']=False
            item['error']='Interrupted command; process group termination attempted';persist();raise
        measure()
        if process.returncode: raise RuntimeError('Command failed: '+argv[0])
        return item
    try:
        run(['sw_vers']);run(['xcrun','clang++','--version'])
        report['sdk_version']=run(['xcrun','--sdk','macosx','--show-sdk-version'])['stdout'].strip()
        sdk=Path(run(['xcrun','--sdk','macosx','--show-sdk-path'])['stdout'].strip())
        headers=sdk/'System/Library/Frameworks/Metal.framework/Headers'
        report['metal_headers']={p.name:{'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
                                 for p in sorted(headers.glob('*.h'))}
        common=['xcrun','clang++','-arch','x86_64','-mmacosx-version-min=15.0','-std=c++17',
                '-Wall','-Wextra','-Wpedantic','-Werror','-O2','-Iinclude']
        objects=[]
        for name in plan['sources']:
            path=Path(name);obj=path.stem+'.o'
            if obj in objects: raise RuntimeError('Duplicate object name')
            args=common+(['-fobjc-arc'] if path.suffix=='.mm' else [])
            # Shared kernel policy units retain their actual C++14 contract.
            if name in ('src/gfx11_compute_product.cpp','userspace/runtime/command_lifecycle.cpp'):
                args=[x.replace('-std=c++17','-std=c++14') for x in args]
            run(args+['-c',name,'-o',obj]);objects.append(obj)
        archive='libHackyNaviMetalFrontend.a';library='libHackyNaviMetalFrontend.dylib'
        run(['xcrun','libtool','-static','-o',archive,*objects])
        links=['-framework','IOKit','-framework','CoreFoundation','-framework','Foundation','-framework','Metal']
        backend_links=links+(['-lHackyNaviGraphicsCompiler','-lHackyNaviColorLayout'] if plan.get('graphics') else [])
        run(common+['-dynamiclib','-Wl,-force_load,'+str(stage/archive),'-L.','-lhackynavi_metal_compiler',
            '-Wl,-rpath,@loader_path','-Wl,-install_name,@rpath/'+library,*backend_links,'-o',library])
        bundle='HackyNaviMetal'
        if plan['device_layer']:
            run(common+['-bundle','-Wl,-force_load,'+str(stage/archive),'-L.','-lhackynavi_metal_compiler',
                '-Wl,-rpath,@loader_path/../Frameworks',*backend_links,'-o',bundle])
            run(['file',bundle]);run(['otool','-L',bundle]);run(['otool','-l',bundle])
        smoke='hackynavi-metal-compute-smoke'
        run(common+['-fobjc-arc','userspace/metal/native/compute-smoke.mm',*links,'-o',smoke])
        render_smoke='hackynavi-metal-render-smoke'
        buffer_smoke='hackynavi-metal-render-buffer-smoke'
        vertex_buffer_smoke='hackynavi-metal-render-vertex-buffer-smoke'
        if plan.get('graphics'):
            run(common+['-fobjc-arc','userspace/metal/native/render-smoke.mm',*links,'-o',render_smoke])
            run(['file',render_smoke]);run(['otool','-L',render_smoke])
            run(common+['-fobjc-arc','userspace/metal/native/render-buffer-smoke.mm',*links,'-o',buffer_smoke])
            run(['file',buffer_smoke]);run(['otool','-L',buffer_smoke])
            run(common+['-fobjc-arc','userspace/metal/native/render-vertex-buffer-smoke.mm',*links,'-o',vertex_buffer_smoke])
            run(['file',vertex_buffer_smoke]);run(['otool','-L',vertex_buffer_smoke])
        run(['file',archive,library,smoke]);run(['otool','-L',library])
        defined=run(['nm','-gU',library])['stdout']
        classes=['HNMetalBuffer','HNMetalLibrary','HNMetalFunction','HNMetalComputePipeline',
                 'HNMetalCommandQueue','HNMetalCommandBuffer','HNMetalCommandEncoder','HNMetalBlitEncoder']
        if plan['device_layer']: classes.extend(['HNMetalDevice','HNMetalArchitecture'])
        if plan.get('graphics'): classes.extend(['HNMetalTexture','HNMetalRenderPipeline','HNMetalRenderEncoder'])
        for name in classes:
            if '_OBJC_CLASS_$_'+name not in defined: raise RuntimeError('Missing linked frontend class: '+name)
        undefined=run(['nm','-u',library])['stdout']
        if plan['device_layer'] and '_OBJC_CLASS_$__MTLDevice' not in undefined: raise RuntimeError('Actual Metal superclass import missing')
        if plan['device_layer']:
            bundle_defined=run(['nm','-gU',bundle])['stdout']
            bundle_undefined=run(['nm','-u',bundle])['stdout']
            for name in classes:
                if '_OBJC_CLASS_$_'+name not in bundle_defined: raise RuntimeError('Missing bundle class: '+name)
            if '_OBJC_CLASS_$__MTLDevice' not in bundle_undefined or '_OBJC_CLASS_$_MTLArchitecture' not in bundle_undefined:
                raise RuntimeError('Actual bundle superclass imports missing')
        report['objects']={p:{'bytes':(stage/p).stat().st_size,'sha256':hashlib.sha256((stage/p).read_bytes()).hexdigest()} for p in objects}
        exports=[archive,library,smoke]
        if plan.get('graphics'): exports.extend([render_smoke,buffer_smoke,vertex_buffer_smoke])
        if plan['device_layer']: exports.append(bundle)
        report['artifacts']={p:{'bytes':(stage/p).stat().st_size,'sha256':hashlib.sha256((stage/p).read_bytes()).hexdigest()} for p in exports}
        with tarfile.open(stage/'artifacts.tar.gz','w:gz') as tar:
            for name in exports: tar.add(stage/name,arcname=name,recursive=False)
        measure()
        report['artifact_archive_sha256']=hashlib.sha256((stage/'artifacts.tar.gz').read_bytes()).hexdigest()
        report['success']=True
    except BaseException as error: report['error']=str(error)
    finally: report['complete']=True;persist()
    return 0 if report['success'] else 1

if __name__=='__main__': raise SystemExit(main())
