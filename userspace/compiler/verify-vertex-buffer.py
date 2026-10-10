#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Decode the separate actual vertex-buffer NGG machine profile, without execution.

This is an offline format/provenance check, not kernel admission or an ISA/GPU
execution model. The driver must independently own bindings and validate bytes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def verify(code, metadata):
    expected = {'abi_version': 1, 'profile': 'gfx1100-vertex-buffer-ngg-v1',
        'compiler': 'Mesa NIR/ACO', 'mesa_commit': '35b085c4c067f198ce0b6437d94678e47b9c645f',
        'target': 'gfx1100', 'stage': 'vertex', 'hardware_stage': 'next_gen_geometry',
        'code_alignment': 256, 'wave_size': 32, 'input_vgprs': 6, 'float_mode': 192,
        'mem_ordered': False, 'wgp_mode': False, 'spi_shader_pgm_rsrc2_gs': 10,
        'ngg_passthrough': True, 'primgen_passthru_no_msg': True, 'early_primitive_export': False,
        'vgt_shader_stages_en': 104931344, 'spi_shader_pos_format': 4,
        'max_workgroup_size': 256, 'vertices_per_primitive': 3, 'position_exports': 1,
        'parameter_exports': 0, 'resource_count': 1, 'user_sgprs': 5, 'base_vertex_sgpr': 12,
        'merged_wave_info_sgpr': 3, 'descriptor_sgpr': 8, 'descriptor_dwords': 4,
        'descriptor_set': 0, 'descriptor_binding': 0, 'buffer_bytes': 48, 'buffer_alignment': 16,
        'buffer_access': 'ReadOnly', 'load_count': 4, 'vertex_stride': 16,
        'component_offsets': [0, 4, 8, 12], 'required_first_vertex': 0, 'required_vertex_count': 3,
        'packed_primitive_vgpr': 0, 'vertex_id_vgpr': 5, 'gs_vgpr_comp_cnt': 0, 'es_vgpr_comp_cnt': 0,
        'scratch_bytes_per_wave': 0, 'lds_bytes': 0, 'attribute_ring_writes': 0,
        'constants_bytes': 0, 'relocations': [], 'gpu_executed': False, 'draw_state_complete': False}
    for key, value in expected.items():
        if type(metadata.get(key)) is not type(value) or metadata[key] != value:
            raise ValueError('Wrong actual vertex-buffer metadata: ' + key)
    for key in ('code_bytes', 'executable_bytes', 'num_sgprs', 'num_vgprs', 'spi_shader_pgm_rsrc1_gs'):
        if type(metadata.get(key)) is not int:
            raise ValueError('Noninteger machine metadata: ' + key)
    if len(code) != 124 or metadata['code_bytes'] != len(code) or metadata['executable_bytes'] != 104:
        raise ValueError('Unsupported machine extent')
    vgprs = metadata['num_vgprs']
    if not 6 <= vgprs <= 24 or not 13 <= metadata['num_sgprs'] <= 128:
        raise ValueError('Unsupported actual register allocation')
    if metadata['spi_shader_pgm_rsrc1_gs'] != 0x2c0000 | ((vgprs - 1) // 8):
        raise ValueError('RSRC1 disagrees with actual allocation')
    words = list(struct.unpack('<31I', code))
    # Actual Mesa merged-NGG ES/primitive-mask prefix. The first branch skips
    # every position load/export when this invocation has no input vertex.
    prefix = [0xbfb50003, 0x84008803, 0x95828003, 0x930000c1, 0xbefe0002, 0xbfa5000e]
    if words[:6] != prefix:
        raise ValueError('Vertex/primitive mask provenance or guarded load branch differs')
    word, second = words[6:8]
    address = word & 255
    if word & 0xffffff00 != 0xd6470000 or address == 0 or address >= vgprs or second != 0x02120a0c:
        raise ValueError('Address must be the actual (base_vertex s12 + VertexID v5) << 4')
    if words[8] != 0xbf850003:
        raise ValueError('Expected actual four-load clause')
    loaded = {}
    listing = []
    for index in range(4):
        word, second = words[9 + index * 2:11 + index * 2]
        offset = word & 0xfff
        destination = (second >> 8) & 255
        # Exact GFX11 MUBUF: load_dword, raw s[8..11], OFFEN only, no
        # resource/format modifiers, inline-zero SOFFSET, real VADDR register.
        if word & 0xfffff000 != 0xe0500000 or offset not in (0, 4, 8, 12) or offset in loaded:
            raise ValueError('Unsupported or duplicate indexed component load')
        if second != 0x80420000 | (destination << 8) | address or destination == 0 or destination >= vgprs:
            raise ValueError('Load does not preserve descriptor/index and allowed register range')
        if destination in loaded.values() or (destination == address and index != 3):
            raise ValueError('A pending result clobbers another load or the live address')
        loaded[offset] = destination
        listing.append({'byte_offset': (9 + index * 2) * 4, 'op': 'buffer_load_dword',
                        'descriptor_sgpr': 8, 'address_vgpr': address, 'component_offset': offset,
                        'destination_vgpr': destination})
    if words[17] != 0xbf8903f7:
        raise ValueError('Actual VM counter must be zero before any position export')
    exported = [(words[19] >> (8 * i)) & 255 for i in range(4)]
    if words[18] != 0xf80008cf or exported != [loaded[i * 4] for i in range(4)]:
        raise ValueError('Position export does not consume ready loaded components in XYZW order')
    if words[20:26] != [0xbf89fff0, 0xbefe0000, 0xbfa50002, 0xf8000941, 0x80808000, 0xbfb00000]:
        raise ValueError('Position completion, actual primitive mask, packed primitive or terminal guard differs')
    if words[26:] != [0xbf9f0000] * 5:
        raise ValueError('Noncanonical ACO padding')
    return {'success': True, 'code_sha256': hashlib.sha256(code).hexdigest(),
            'executable_bytes': 104, 'descriptor_sgprs': [8, 9, 10, 11], 'base_vertex_sgpr': 12,
            'vertex_id_vgpr': 5, 'address_vgpr': address, 'loads': listing,
            'load_counter_wait': {'byte_offset': 68, 'vmcnt': 0}, 'position_export_offset': 72,
            'primitive_export_offset': 92, 'vertex_branch_target': 80, 'primitive_branch_target': 100,
            'required_draw': {'first_vertex': 0, 'vertices': 3, 'instances': 1, 'base_instance': 0},
            'required_raw_descriptor_bytes': 48, 'gpu_executed': False,
            'scope': 'Offline machine format/mask/address/component/wait provenance; no instruction execution.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('product', type=Path)
    parser.add_argument('report', type=Path)
    args = parser.parse_args()
    if args.report.exists(): parser.error('Report must be fresh')
    result = verify((args.product / 'shader.bin').read_bytes(), json.loads((args.product / 'shader.json').read_text()))
    args.report.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'success': True, 'code_sha256': result['code_sha256']}))


if __name__ == '__main__': main()
