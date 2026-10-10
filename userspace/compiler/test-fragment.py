#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real SPIRV-Tools -> NIR/ACO PS tests and bounded binary export verification."""
import argparse
import copy
import ctypes as c
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('verify_fragment', HERE / 'verify-fragment.py')
decoder = importlib.util.module_from_spec(spec); spec.loader.exec_module(decoder)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        parser.error('Evidence directory must be new')
    output.mkdir(parents=True)
    assembler = ROOT / '.tools/userspace/build-spirv-tools/tools/spirv-as.exe'
    validator = ROOT / '.tools/userspace/build-spirv-tools/tools/spirv-val.exe'
    compiler = args.compiler.resolve()
    tools = json.loads((ROOT / 'build/metal-explicit-tools-local/explicit-tools.json').read_text())['tools']
    for name in ('compiler', 'air_dis', 'spirv_val'):
        if sha(Path(tools[name]['path'])) != tools[name]['sha256']:
            raise RuntimeError('Frozen explicit-tools compiler dependency changed: ' + name)
    report = {'success': False, 'gpu_executed': False, 'metal_objects_created': False,
              'tools': {name: {'path': str(p), 'sha256': sha(p)} for name, p in
                        [('assembler', assembler), ('validator', validator), ('fragment_compiler', compiler)]},
              'sources': {str(p.relative_to(ROOT)): sha(p) for p in
                          [HERE / n for n in ('fragment.c','hackynavi_gfx11_fragment.h','verify-fragment.py','test-fragment.py','fixtures/fragment/constant.spvasm')]},
              'commands': [], 'positive': [], 'negative': [], 'machine_rejections': [], 'checks': 0}

    def check(condition, message):
        report['checks'] += 1
        if not condition:
            raise AssertionError(message)

    def run(command, success=True):
        result = subprocess.run([str(x) for x in command], capture_output=True, text=True, timeout=30)
        report['commands'].append({'args': [str(x) for x in command], 'returncode': result.returncode,
                                   'stdout': result.stdout, 'stderr': result.stderr})
        check((result.returncode == 0) == success, str(command) + result.stderr)
        return result

    template = (HERE / 'fixtures/fragment/constant.spvasm').read_text()

    def compile_text(name, text, error=None, entry='main'):
        directory = output / name; directory.mkdir()
        source, spirv = directory / 'source.spvasm', directory / 'shader.spv'
        source.write_text(text)
        run([assembler, '--target-env', 'vulkan1.2', source, '-o', spirv])
        run([validator, '--target-env', 'vulkan1.2', spirv])
        product = directory / 'product'; product.mkdir()
        result = run([compiler, spirv, entry, product], error is None)
        if error is not None:
            check(error in result.stderr, 'Precise unsupported contract: ' + name + result.stderr)
            check(not list(product.iterdir()), 'Rejected shader published partial output')
            report['negative'].append({'name': name, 'spirv_sha256': sha(spirv), 'diagnostic': result.stderr.strip()})
            return
        code = (product / 'shader.bin').read_bytes()
        metadata = json.loads((product / 'shader.json').read_text())
        decoded = decoder.verify(code, metadata)
        (directory / 'disassembly.json').write_text(json.dumps(decoded, indent=2) + '\n')
        check('not supported in the current configuration' in (product / 'aco-diagnostics.txt').read_text(),
              'ACO fallback is retained and explicitly identified, never mislabelled disassembly')
        report['positive'].append({'name': name, 'spirv_sha256': sha(spirv), 'code_sha256': sha(product / 'shader.bin'),
                                   'metadata': metadata, 'decoded': decoded})
        return code, metadata

    try:
        invalid = output / 'invalid.spv'; invalid.write_bytes(b'not SPIR-V')
        run([validator, '--target-env', 'vulkan1.2', invalid], False)
        baseline = compile_text('white', template)
        for name, values in [('half', [0.5]*4), ('literal', [0.25]*4), ('distinct', [1,0.5,2,4]),
                             ('signed', [-1,-0.5,-2,-4]), ('mixed_literals', [0.125,0.25,0.75,1]), ('zero', [0]*4)]:
            constants = '\n'.join(f'%value{i} = OpConstant %float {value}' for i, value in enumerate(values))
            text = template.replace('%one = OpConstant %float 1', constants).replace('%one %one %one %one', '%value0 %value1 %value2 %value3')
            code, meta = compile_text(name, text)
            expected = [struct.unpack('<I', struct.pack('<f', v))[0] for v in values]
            check(meta['color_bits'] == expected, 'NIR source color remains exact')
            check(code != baseline[0], 'Changed source color changes actual machine code')
        compile_text('location1', template.replace('Location 0', 'Location 1'), 'exactly one float32 vec4')
        compile_text('scalar', template.replace('%output = OpTypePointer Output %rgba','%output = OpTypePointer Output %float').replace('OpStore %color %white','OpStore %color %one'), 'exactly one float32 vec4')
        compile_text('no_output', template.replace(' "main" %color',' "main"').replace('OpDecorate %color Location 0','').replace('%color = OpVariable %output Output','').replace('OpStore %color %white',''), 'missing color output')
        compile_text('early_tests', template.replace('OpExecutionMode %main OriginUpperLeft','OpExecutionMode %main OriginUpperLeft\nOpExecutionMode %main EarlyFragmentTests'), 'unsupported capability, execution mode')
        compile_text('specialization', template.replace('%one = OpConstant %float 1','%one = OpSpecConstant %float 1').replace('OpDecorate %color Location 0','OpDecorate %color Location 0\nOpDecorate %one SpecId 0'), 'unsupported capability, execution mode or specialization')
        compile_text('discard', template.replace('OpStore %color %white\nOpReturn','OpKill'), 'requires one full constant RGBA output')
        compile_text('wrong_entry', template, 'entrypoint', entry='missing')

        # Real owned metallib extraction/translation through the existing C ABI.
        # The SDL fragment genuinely uses an interpolated varying and must be
        # rejected by the narrower PS backend without erasing that input.
        lib = c.CDLL(tools['compiler']['path'])
        ptr, size, u32 = c.c_void_p, c.c_size_t, c.c_uint32
        class Tools(c.Structure):
            _fields_ = [('struct_size',u32),('version',u32),('air_dis_path',ptr),('spirv_val_path',ptr)]
        lib.hn_shader_compile_with_tools.argtypes = [ptr,ptr,size,u32,u32,c.c_char_p,u32,u32,u32,u32,u32]
        lib.hn_shader_compile_with_tools.restype = ptr
        for name, arguments, result in [('error',[ptr],c.c_char_p),('words',[ptr,c.POINTER(size)],ptr),
                                        ('spirv_entry_point',[ptr],c.c_char_p),('reflection',[ptr],c.c_char_p)]:
            fn = getattr(lib,'hn_shader_result_'+name);fn.argtypes=arguments;fn.restype=result
        lib.hn_shader_result_free.argtypes=[ptr];lib.hn_shader_result_free.restype=None
        air = c.create_string_buffer(os.fsencode(tools['air_dis']['path']))
        val = c.create_string_buffer(os.fsencode(tools['spirv_val']['path']))
        config = Tools(c.sizeof(Tools),1,c.cast(air,ptr),c.cast(val,ptr))
        fixture = ROOT / 'userspace/metal/tests/fixtures/metallib/sdl-2.30.11-macos.metallib'
        raw = fixture.read_bytes(); memory = c.create_string_buffer(raw)
        shader = lib.hn_shader_compile_with_tools(c.byref(config),memory,len(raw),2,2,b'SDL_Solid_fragment',1,1,1,0,1)
        try:
            check(bool(shader) and lib.hn_shader_result_error(shader)==b'', 'Actual SDL AIR fragment translates')
            count=size(); words=lib.hn_shader_result_words(shader,c.byref(count))
            check(bool(words) and count.value>5, 'Real fragment SPIR-V present')
            entry=lib.hn_shader_result_spirv_entry_point(shader).decode()
            directory=output/'sdl_fragment';directory.mkdir()
            spirv=directory/'shader.spv';spirv.write_bytes(c.string_at(words,count.value*4))
            (directory/'reflection.json').write_bytes(lib.hn_shader_result_reflection(shader))
            run([validator,'--target-env','vulkan1.2',spirv])
            product=directory/'product';product.mkdir()
            result=run([compiler,spirv,entry,product],False)
            check('varying/system inputs are unsupported' in result.stderr,'Actual SDL varying rejected precisely')
            check(not list(product.iterdir()),'SDL rejection publishes no fake color shader')
            report['negative'].append({'name':'actual SDL_Solid_fragment','fixture_sha256':sha(fixture),'spirv_sha256':sha(spirv),'diagnostic':result.stderr.strip()})
            report['sdl_tools']=tools
        finally:
            if shader:lib.hn_shader_result_free(shader)

        code, metadata = baseline
        def reject_machine(name, candidate=code, meta=metadata):
            try:decoder.verify(candidate,meta)
            except ValueError as error:
                report['machine_rejections'].append({'name':name,'diagnostic':str(error)})
                check(True,name);return
            raise AssertionError('Accepted malformed machine product: '+name)
        for length in range(len(code)):
            reject_machine('truncation '+str(length),code[:length])
        for offset,value,name in [(0,0x7e000200,'uninitialized SGPR'),(4,0xf800000f,'missing export DONE'),
                                  (4,0xf800081f,'wrong MRT'),(4,0xf8000807,'missing alpha'),
                                  (4,0xf800280f,'unexpected ROW_EN'),(8,0xffffffff,'uninitialized export VGPRs'),
                                  (12,0xbf800000,'missing program end'),(16,0,'invalid padding')]:
            changed=bytearray(code);struct.pack_into('<I',changed,offset,value);reject_machine(name,bytes(changed))
        for key,value in [('color_bits',[0,0,0,0]),('spi_ps_input_ena',0),('spi_ps_in_control',0),
                          ('num_vgprs',0),('spi_shader_pgm_rsrc1_ps',0),('resource_count',1),('wave_size',64)]:
            changed=copy.deepcopy(metadata);changed[key]=value;reject_machine('metadata '+key,meta=changed)
        report['success']=True
    finally:
        (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'success':True,'checks':report['checks'],'positive':len(report['positive']),'negative':len(report['negative']),
                      'machine_rejections':len(report['machine_rejections']),'report_sha256':sha(output/'report.json')}))


if __name__ == '__main__':
    main()
