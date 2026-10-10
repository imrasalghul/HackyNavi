#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host-only GFX11 draw-state oracle using real pinned Mesa functions.

Reuses exact existing Mesa archives. Never opens a GPU or invokes a remote host.
The actual archive bytes are bound; their original reconstruction is not rerun.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import struct

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / '.tools/userspace/build-mesa-compiler'
STAGE = ROOT / '.tools/userspace/mesa-compiler-source'
PIN = '35b085c4c067f198ce0b6437d94678e47b9c645f'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        parser.error('Output must be new; earlier evidence is immutable')
    helper = ROOT / 'userspace/compiler/build-fragment.py'
    spec = importlib.util.spec_from_file_location('fragment_build_helpers', helper)
    helpers = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helpers)
    prior_path = ROOT / 'build/gfx11-fragment-local-6/report.json'
    prior_bytes = prior_path.read_bytes()
    prior = json.loads(prior_bytes)
    if not prior['success'] or prior['mesa_commit'] != PIN:
        raise RuntimeError('Expected successful reviewed actual Mesa archive build')
    records_path = BUILD / 'compile_commands.json'
    ninja_path = BUILD / 'build.ninja'
    manifest_path = ROOT / 'userspace/compiler/source-manifest.json'
    for path, key in [(helper, None), (records_path, 'compile_database_sha256'),
                      (ninja_path, 'build_graph_sha256'), (manifest_path, 'source_manifest_sha256')]:
        expected = prior['source_files'][str(helper.relative_to(ROOT))] if key is None else prior[key]
        if sha(path) != expected:
            raise RuntimeError('Reviewed Mesa build input changed: ' + str(path))
    manifest = json.loads(manifest_path.read_text())
    if manifest['commit'] != PIN:
        raise RuntimeError('Wrong source manifest pin')
    materialized = {item['path']: item['sha256'] for item in manifest['files']}
    zig = ROOT / '.tools/zig-x86_64-windows-0.17.0/zig.exe'
    if sha(zig) != prior['compiler_sha256']:
        raise RuntimeError('Actual reviewed compiler changed')
    records = json.loads(records_path.read_text())
    record = [r for r in records if r.get('output') == 'src/amd/common/libamd_common.a.p/ac_barrier.c.obj']
    if len(record) != 1 or Path(record[0]['directory']).resolve() != BUILD:
        raise RuntimeError('Expected actual ac_barrier compile record')
    command = helpers.split_windows(record[0]['command'])
    if Path(command[0]).resolve() != zig or command[1] != 'cc':
        raise RuntimeError('Unexpected Mesa compiler')
    stripped = []
    index = 2
    while index < len(command):
        item = command[index]
        if item in ('-o', '-MQ', '-MF'):
            index += 2
            continue
        if item in ('-MD', '-c') or item == record[0]['file']:
            index += 1
            continue
        if item.startswith('-I'):
            stripped.extend(['-isystem', item[2:]])
        else:
            stripped.append(item.replace('-D_WIN32_WINNT=0x0A00', '-D_WIN32_WINNT=0x0a00'))
        index += 1
    lines = ninja_path.read_text().splitlines()
    start = next(i for i, line in enumerate(lines) if line.startswith('build hackynavi-compiler/hackynavi-aco.exe:'))
    if not lines[start + 1].lstrip().startswith('LINK_ARGS = '):
        raise RuntimeError('Unexpected actual link record')
    link_args = helpers.split_windows('link ' + lines[start + 1].split(' = ', 1)[1])[1:]
    archives = [(BUILD / item).resolve() for item in link_args if item.endswith('.a')]
    if len(archives) != 12 or any(sha(p) != prior['archives'][str(p.relative_to(ROOT))] for p in archives):
        raise RuntimeError('Reviewed actual reused archive changed')
    oracle = ROOT / 'tests/gfx11_graphics_draw_mesa_oracle.c'
    sources = [ROOT / name for name in ('tests/gfx11_graphics_draw_mesa_oracle.c',
               'scripts/check-gfx11-graphics-draw-oracle.py', 'docs/gfx11-graphics-draw-contract.md',
               'include/hackynavi/gfx11_graphics_draw.hpp','src/gfx11_graphics_draw.cpp',
               'tests/gfx11_graphics_draw_tests.cpp','include/hackynavi/gfx11_graphics_product.hpp',
               'src/gfx11_graphics_product.cpp','include/hackynavi/gfx11_graphics_barrier.hpp',
               'src/gfx11_graphics_barrier.cpp','include/hackynavi/gfx11_color_target.h',
               'src/gfx11_color_target.cpp','include/hackynavi/isolated_compute.hpp',
               'include/hackynavi/gfx_compute.hpp')]
    fixtures = [ROOT / name for name in (
        'build/apple-graphics-integration-1/air-vertex/product/shader.bin',
        'build/apple-graphics-integration-1/air-fragment/product/shader.bin',
        'build/gfx11-ngg-tests-3/literal/product/shader.bin',
        'build/gfx11-fragment-tests-4/literal/product/shader.bin',
        'build/gfx11-ngg-tests-3/reverse/product/shader.bin',
        'build/gfx11-fragment-tests-4/half/product/shader.bin')]
    expected = ['ffa5d5142a6f8a5786296ba27f92e631c236bfd35161279b28919dd8997c9c61',
        '15f6f976c7b77857915d8254f55d028c8413bcf74b96f8403767f1d90631f36c',
        '69619a05dddf65ae1d50c406cf0a395b32c28e92e02eafa2122a29c99ef02b05',
        '340aea59bb687732a3e16305fd6a61b169838114630137bdfc883f6054b0d9e8',
        'c969de47af807b6c89ce36abb60514fc3b81bb4ef54ed36ebb4246d73eded247',
        'be6733d4607dd5517aa33c0403e07bd032025ae3a3b16f1819c55716de45bd91']
    if any(sha(p)!=value for p,value in zip(fixtures,expected)):
        raise RuntimeError('An actual immutable compiler product changed')
    sources.extend(fixtures)
    sources.append(ROOT/'.research/linux/gfx_v11_0.c')
    references = [ROOT / '.research/mesa' / name for name in ('src/amd/common/ac_barrier.c',
        'src/amd/common/ac_barrier.h', 'src/amd/common/ac_cmdbuf.c', 'src/amd/common/ac_cmdbuf_cp.c',
        'src/amd/common/ac_cmdbuf_base.h', 'src/amd/common/ac_gpu_info.c',
        'src/amd/common/ac_shader_util.c', 'src/amd/common/ac_binary.c',
        'src/amd/common/ac_formats.c', 'src/amd/common/ac_surface.c', 'src/amd/common/ac_descriptors.c',
        'src/amd/common/ac_guardband.c', 'src/amd/common/ac_guardband.h',
        'src/amd/vulkan/radv_queue.c', 'src/amd/vulkan/radv_shader.c',
        'src/amd/vulkan/radv_shader_info.c', 'src/amd/vulkan/radv_cmd_buffer.c', 'src/amd/vulkan/radv_device.c',
        'src/amd/vulkan/radv_pipeline_graphics.c',
        'src/amd/registers/pkt3.json', 'src/amd/registers/gfx11.json', 'src/amd/packets/cp_pm4_table_data_gfx11.json')]
    for path in references:
        if sha(path) != materialized[path.relative_to(ROOT / '.research/mesa').as_posix()]:
            raise RuntimeError('Pinned source reference changed')
    bound = [*sources, *references, helper, prior_path, records_path, ninja_path, manifest_path, zig, *archives]
    report = {'success': False, 'gpu_executed': False, 'remote_executed': False,
              'mesa_commit': PIN, 'files': {str(p.relative_to(ROOT)): sha(p) for p in bound},
              'archive_provenance': prior['archive_provenance'], 'commands': []}
    output.mkdir(parents=True)
    (output / 'prior-build-report.json').write_bytes(prior_bytes)
    viewport_source=ROOT/'.research/mesa/src/amd/vulkan/radv_pipeline_graphics.c'
    viewport_text=viewport_source.read_text()
    viewport_start=viewport_text.index('void\nradv_get_viewport_xform(')
    viewport_end=viewport_text.index('\n}\n',viewport_start)+3
    viewport_function=viewport_text[viewport_start:viewport_end]
    viewport_header=output/'mesa_viewport_xform.h'
    viewport_header.write_text('#include <vulkan/vulkan_core.h>\n'
        'void radv_get_viewport_xform(const VkViewport *viewport, float scale[3], float translate[3]);\n'+viewport_function)
    report['viewport_oracle']={'source':str(viewport_source.relative_to(ROOT)),
        'source_sha256':sha(viewport_source),'first_line':viewport_text[:viewport_start].count('\n')+1,
        'function_sha256':hashlib.sha256(viewport_function.encode()).hexdigest(),
        'extracted_header_sha256':sha(viewport_header),
        'logical_convention':'Metal positive dimensions; NDC Y +1 top, negative hardware Y scale; Z unchanged 0..1'}

    def run(command, cwd=ROOT):
        result = subprocess.run([str(x) for x in command], cwd=cwd, text=True, capture_output=True, timeout=120)
        report['commands'].append({'args': [str(x) for x in command], 'returncode': result.returncode,
                                   'stdout': result.stdout, 'stderr': result.stderr})
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        return result.stdout

    def dependency_snapshot(path):
        result = {}
        for item in helpers.dependencies(path):
            value = sha(item)
            if item.is_relative_to(STAGE):
                if materialized.get(item.relative_to(STAGE).as_posix()) != value:
                    raise RuntimeError('Actual Mesa header differs from manifest')
                kind = 'manifest_verified_mesa_source'
            elif item.is_relative_to(BUILD):
                kind = 'generated_build_header'
            elif item.is_relative_to(zig.parent):
                kind = 'toolchain_header'
            elif item == oracle:
                kind = 'authored_oracle_caller'
            elif item == viewport_header:
                if sha(item)!=report['viewport_oracle']['extracted_header_sha256']:
                    raise RuntimeError('Verbatim viewport extraction changed')
                kind = 'verbatim_pinned_source_extraction'
            else:
                raise RuntimeError('Unclassified actual dependency: ' + str(item))
            result[item.relative_to(ROOT).as_posix()] = {'sha256': value, 'kind': kind}
        return result

    try:
        common = [zig, 'cc', *stripped, '-I'+str(output), '-Werror', '-Wextra']
        run([*common, '-M', '-MT', 'fragment.obj', '-MF', output / 'oracle-before.d', oracle], BUILD)
        report['oracle_dependencies'] = dependency_snapshot(output / 'oracle-before.d')
        run([*common, '-MD', '-MT', 'fragment.obj', '-MF', output / 'oracle-compiled.d', '-c', oracle,
             '-o', output / 'mesa-oracle.obj'], BUILD)
        if dependency_snapshot(output / 'oracle-compiled.d') != report['oracle_dependencies']:
            raise RuntimeError('Oracle dependencies changed during actual compile')
        run([zig, 'c++', '-o', output / 'mesa-oracle.exe', output / 'mesa-oracle.obj', *link_args], BUILD)
        cases = []
        # Explicit numerical boundary scenarios, never purported hardware observations.
        checks = 0
        for se, rb, cu, gb, steering, hi, vs, ps in (
            (6,24,16,0x40,0,1,124,36), (6,24,16,0x41,0,1,124,36),
            (6,24,16,0x344,0,1,124,36), (6,24,16,0x446,0,1,124,36),
            (1,4,2,0x41,0,0,4,4), (2,8,4,0x142,0,0x100,128,128),
            (4,16,8,0x243,0,0x7fff,132,132), (6,24,16,0x545,0,1,1004,1004),
            (6,24,2,0x446,0,1,124,36), (6,24,4,0x446,0,1,124,36),
            (6,24,8,0x446,0,1,124,36), (6,24,12,0x446,0,1,124,36)):
            result = subprocess.run([str(output / 'mesa-oracle.exe'), *map(str,(se,rb,cu,gb,steering,hi,vs,ps))],
                                    capture_output=True, text=True, timeout=15)
            report['commands'].append({'args': [str(output / 'mesa-oracle.exe'), *map(str,(se,rb,cu,gb,steering,hi,vs,ps))],
                                       'returncode': result.returncode, 'stderr': result.stderr})
            if result.returncode:
                raise RuntimeError(result.stderr)
            case = json.loads(result.stdout)
            d = case['derived']
            def check(condition):
                nonlocal checks
                checks += 1
                if not condition: raise RuntimeError('Actual Mesa oracle invariant failed')
            check(not case['hardware_observation'])
            check(d['attribute_per_se'] == 22*65536 and d['attribute_total'] == 22*65536*se)
            check(d['num_tile_pipes'] == 1 << (gb&7))
            check(d['es_vertices'] == d['gs_primitives'] == d['output_vertices'] == 128)
            check(d['esgs_lds_dwords'] == d['output_lds_dwords'] == 0)
            check(d['late_alloc'] == (0 if cu<=2 else 63))
            check(d['ngg_cu_mask'] == (65535 if cu<=2 else 65533))
            check(d['ps_cu_mask'] == (1<<cu)-1)
            check(d['vs_prefetch_bytes'] == ((vs+192+127)//128)*128)
            check(d['ps_prefetch_bytes'] == ((ps+192+127)//128)*128)
            check(d['vs_prefetch_field'] == d['vs_prefetch_bytes']//128)
            check(d['ps_prefetch_field'] == d['ps_prefetch_bytes']//128)
            check(len(case['attribute_idle_and_ring']) == 22)
            check(len(case['graphics_completion']) == 16)
            check(len(case['instruction_visibility']) > 0)
            check(len(case['common_preamble']) > 100)
            check(len({r['name'] for r in case['registers']}) == len(case['registers']))
            cases.append(case)
        (output / 'mesa-draw-state.json').write_text(json.dumps({'cases': cases}, indent=2)+'\n')
        report['oracle_cases'] = len(cases)
        report['oracle_checks'] = checks
        report['oracle_data_sha256'] = sha(output / 'mesa-draw-state.json')
        cpp_sources=[ROOT / name for name in ('src/gfx11_graphics_draw.cpp','src/gfx11_graphics_product.cpp',
             'src/gfx11_graphics_barrier.cpp','src/gfx11_color_target.cpp','tests/gfx11_graphics_draw_tests.cpp')]
        cpp=[zig,'c++','-std=c++14','-Wall','-Wextra','-Wpedantic','-Werror','-fno-exceptions','-fno-rtti',
             '-I'+str(ROOT/'include'),*cpp_sources]
        run([*cpp,'-O2','-o',output/'tests.exe'])
        run([output/'tests.exe'])
        run([*cpp,'-O1','-g','-fsanitize=undefined','-fno-sanitize-recover=all','-o',output/'tests-ubsan.exe'])
        run([output/'tests-ubsan.exe'])
        run([zig,'c++','-target','x86_64-macos','-mmacosx-version-min=15.0','-std=c++14','-O2',
             '-Wall','-Wextra','-Wpedantic','-Werror','-Wframe-larger-than=2048','-fno-exceptions','-fno-rtti',
             '-ffreestanding','-I'+str(ROOT/'include'),'-c',ROOT/'src/gfx11_graphics_draw.cpp',
             '-o',output/'gfx11_graphics_draw-macos.o'])
        kernel=[zig,'c++','-target','x86_64-macos.15.0','-std=c++14','-mkernel','-fapple-kext',
            '-DKERNEL','-DKERNEL_EXTENSION','-nostdinc','-nostdlibinc','-I',ROOT/'.research/MacKernelSDK/Headers',
            '-I',ROOT/'include','-fno-exceptions','-fno-rtti','-fno-builtin','-fno-common','-fno-stack-protector',
            '-mno-red-zone','-mno-mmx','-mno-sse','-mno-sse2','-Wall','-Wextra','-Werror',
            '-Wno-unused-parameter','-Wframe-larger-than=2048','-O2']
        run([*kernel,'-M','-MT','fragment.obj','-MF',output/'kernel-before.d',ROOT/'src/gfx11_graphics_draw.cpp'])
        kernel_deps=helpers.dependencies(output/'kernel-before.d')
        report['kernel_dependencies']={p.relative_to(ROOT).as_posix():sha(p) for p in kernel_deps}
        run([*kernel,'-MD','-MT','fragment.obj','-MF',output/'kernel-compiled.d','-c',ROOT/'src/gfx11_graphics_draw.cpp',
             '-o',output/'gfx11_graphics_draw-kernel.o'])
        if (helpers.dependencies(output/'kernel-compiled.d')!=kernel_deps or
            any(sha(ROOT/name)!=value for name,value in report['kernel_dependencies'].items())):
            raise RuntimeError('Actual kernel-header inputs changed')
        report['kernel_frame_limit']=2048

        def registers(data):
            result=[];at=0
            while at<len(data):
                if data[at]==0xffff1000:
                    at+=1;continue
                header=data[at];count=((header>>16)&0x3fff)+1;op=(header>>8)&255
                if header>>30!=3 or at+count>=len(data):raise RuntimeError('Invalid packet boundary')
                if op in (0x69,0x76,0x79):
                    address={0x69:0x28000,0x76:0xb000,0x79:0x30000}[op]+data[at+1]*4
                    result.extend((address+i*4,v)for i,v in enumerate(data[at+2:at+count+1]))
                at+=count+1
            return result
        def find(haystack,needle):
            matches=[i for i in range(len(haystack)-len(needle)+1)if haystack[i:i+len(needle)]==needle]
            if len(matches)!=1:raise RuntimeError('Expected one exact actual-Mesa slice')
            return matches[0]
        database=ROOT/'.research/mesa/src/amd/registers/gfx11.json'
        known={r['map']['at'] for r in json.loads(database.read_text())['register_mappings']
               if r['map']['to']=='mm' and 'gfx11' in r['chips']}
        # These two omissions have exact pinned GFX11 emitters, so database
        # membership alone must not suppress their real required state writes.
        exceptions={0x286dc:{'name':'SPI_BARYC_SSAA_CNTL',
            'source':'.research/mesa/src/amd/common/ac_cmdbuf.c','line':502,
            'call':'ac_pm4_set_reg(pm4, R_0286DC_SPI_BARYC_SSAA_CNTL, 0);'},
            0x3098c:{'name':'GE_VRS_RATE','source':'.research/mesa/src/amd/vulkan/radv_cmd_buffer.c',
            'line':4820,'call':'radeon_set_uconfig_reg(R_03098C_GE_VRS_RATE, S_03098C_RATE_X(rate_x) | S_03098C_RATE_Y(rate_y));'}}
        for address,proof in exceptions.items():
            if address in known or proof['call'] not in (ROOT/proof['source']).read_text():
                raise RuntimeError('GFX11 database exception evidence changed')
        removed={0x28c64,0x28c68,0x28c7c,0x28c80,0x28c84,0x28c88,0x28e60,0x28e80}
        if removed & (known|exceptions.keys()):raise RuntimeError('GFX10-only regression address admitted')
        def unknown_registers(regs):return sorted({a for a,_ in regs}-known-exceptions.keys())
        old_stream=ROOT/'build/gfx11-graphics-draw-local-2/draw-0-0.bin'
        old_bytes=old_stream.read_bytes()
        old_unknown=unknown_registers(registers(list(struct.unpack('<'+'I'*(len(old_bytes)//4),old_bytes))))
        if set(old_unknown)!=removed:raise RuntimeError('Historical CB regression not detected exactly')
        report['register_database']={'path':str(database.relative_to(ROOT)),'sha256':sha(database),
            'entries':len(known),'narrow_source_exceptions':exceptions,
            'historical_rejected_stream':str(old_stream.relative_to(ROOT)),
            'historical_sha256':sha(old_stream),'historical_unknown_addresses':old_unknown}
        comparisons=[]
        for i,(se,cu,gb,hi,width,height) in enumerate((
            (6,16,0x545,1,3840,2160),(1,2,0x41,0,1,1),(2,4,0x142,256,63,65),
            (4,8,0x243,32767,16384,16383),(6,12,0x446,1,4099,4097))):
            for pair in range(3):
                binary=output/f'draw-{i}-{pair}.bin'
                measured=json.loads(run([output/'tests.exe','--emit',binary,se,cu,gb,hi,'actual',width,height,
                                         fixtures[pair*2],fixtures[pair*2+1]]))
                actual=json.loads(run([output/'mesa-oracle.exe',se,se*4,cu,gb,0,hi,
                                      measured['vertex_executable'],measured['fragment_executable'],width,height]))
                data=list(struct.unpack('<'+'I'*measured['words'],binary.read_bytes()))
                regs=registers(data)
                unknown=unknown_registers(regs)
                if unknown:raise RuntimeError('Unproven GFX11 register writes: '+repr(unknown))
                common_at=find(regs,registers(actual['common_preamble']))
                fixed_at=find(regs,[(r['address'],r['value'])for r in actual['registers']])
                color_at=find(regs,[(r['address'],r['value'])for r in actual['color_registers']])
                viewport_at=find(regs,[(r['address'],r['value'])for r in actual['viewport_registers']])
                old_path=ROOT/f'build/gfx11-graphics-draw-local-4/draw-{i}-{pair}.bin'
                old_data=list(struct.unpack('<'+'I'*measured['words'],old_path.read_bytes()))
                delta=[n for n,(old,new) in enumerate(zip(old_data,data)) if old!=new]
                if len(delta)!=1 or old_data[delta[0]]^data[delta[0]]!=0x80000000:
                    raise RuntimeError('Viewport correction must change exactly one Y sign bit in prior actual stream')
                old_regs=dict(registers(old_data)); new_regs=dict(regs)
                if (old_regs[0x28444]&0x80000000 or not new_regs[0x28444]&0x80000000 or
                    old_regs[0x2844c]!=new_regs[0x2844c] or old_regs[0x28450]!=new_regs[0x28450]):
                    raise RuntimeError('Expected one Y inversion with unchanged Z')
                ring_at=find(data,actual['attribute_idle_and_ring'])
                visibility_at=find(data,actual['instruction_visibility'])
                completion_at=find(data,actual['graphics_completion'])
                draw_at=find(data,[0xc0012d00,3,2])
                if not (ring_at<visibility_at<draw_at<completion_at and common_at<fixed_at<color_at<viewport_at):
                    raise RuntimeError('Wrong state/visibility/draw/completion ordering')
                comparisons.append({'stream':binary.name,'sha256':sha(binary),'input':actual['input'],
                    'dimensions':[width,height],'words':measured['words'],'common_registers':len(registers(actual['common_preamble'])),
                    'fixed_registers':len(actual['registers']),'all_four_real_mesa_slices_equal':True,
                    'color_registers':len(actual['color_registers']),'actual_color_fields_and_gfx11_sequence_equal':True,
                    'viewport_registers':len(actual['viewport_registers']),'actual_radv_negative_height_viewport_equal':True,
                    'prior_positive_y_stream_sha256':sha(old_path),'only_prior_stream_change':'one hardware Y scale sign bit',
                    'total_register_writes':len(regs),'unique_register_addresses':len({a for a,_ in regs}),
                    'all_register_addresses_proven':True})
        report['canonical_comparisons']=comparisons
        report['scope']='Canonical fixed draw bytes and rejection tests, compared with actual Mesa host functions and named RADV formulas. Numeric topology tests are not hardware observations; no GPU operation or native ownership is established.'
        report['artifacts'] = {p.name: sha(p) for p in output.iterdir() if p.suffix in ('.exe', '.obj', '.o', '.bin')}
        report['success'] = True
    finally:
        report['inputs_unchanged'] = all(sha(ROOT / p) == value for p, value in report['files'].items())
        report['dependencies_unchanged'] = bool(report.get('oracle_dependencies')) and all(
            sha(ROOT / p) == value['sha256'] for p, value in report.get('oracle_dependencies', {}).items())
        report['kernel_dependencies_unchanged']=bool(report.get('kernel_dependencies')) and all(
            sha(ROOT/p)==value for p,value in report.get('kernel_dependencies',{}).items())
        report['success'] &= report['inputs_unchanged'] and report['dependencies_unchanged'] and report['kernel_dependencies_unchanged']
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    if not report['success']:
        raise RuntimeError('Graphics draw oracle failed')
    print(json.dumps({'success': True, 'report': str(output / 'report.json'), 'sha256': sha(output / 'report.json')}))


if __name__ == '__main__':
    main()
