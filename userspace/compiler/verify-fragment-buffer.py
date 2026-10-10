#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Decode the real GFX11 buffer-color product and prove load/export provenance.

Offline compiler check only. This is not a kernel admission policy, GPU
execution, or a substitute for native resource ownership and synchronization.
Encoding sources: pinned Mesa aco_assembler.cpp:554,988; aco_opcodes.py:777,
798,1714; aco_ir.cpp:1452 and aco_form_hard_clauses.cpp.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def verify(code, metadata):
    if not code or len(code) > 1024 or len(code) % 4:
        raise ValueError('Invalid code extent')
    expected = {
        'abi_version': 1, 'profile': 'gfx1100-buffer-rgba32-fragment-v1',
        'compiler': 'Mesa NIR/ACO', 'mesa_commit': '35b085c4c067f198ce0b6437d94678e47b9c645f',
        'target': 'gfx1100', 'stage': 'fragment', 'hardware_stage': 'pixel_shader',
        'wave_size': 32, 'code_alignment': 256, 'num_sgprs': 108, 'num_vgprs': 24,
        'input_vgprs': 1, 'float_mode': 192, 'wgp_mode': False, 'mem_ordered': False,
        'spi_shader_pgm_rsrc1_ps': 0x2c0002, 'spi_shader_pgm_rsrc2_ps': 8,
        'spi_ps_input_ena': 128, 'spi_ps_input_addr': 128, 'spi_shader_col_format': 9,
        'cb_shader_mask': 15, 'spi_ps_in_control': 32768, 'spi_shader_z_format': 0,
        'resource_count': 1, 'varying_count': 0, 'user_sgprs': 4, 'descriptor_sgpr_base': 0,
        'buffer_set': 0, 'buffer_binding': 0, 'buffer_read_offset': 0, 'buffer_read_bytes': 16,
        'nir_load_count': 4, 'scratch_bytes_per_wave': 0, 'lds_bytes': 0,
        'constants_bytes': 0, 'relocations': [], 'gpu_executed': False, 'draw_state_complete': False}
    for key, value in expected.items():
        if type(metadata.get(key)) is not type(value) or metadata[key] != value:
            raise ValueError('Incorrect fragment-buffer metadata: ' + key)
    if 'color_bits' in metadata:
        raise ValueError('Buffer output is not a constant-color product')
    executable = metadata.get('executable_bytes')
    if type(executable) is not int or not 0 < executable <= len(code) or executable % 4:
        raise ValueError('Invalid executable extent')
    if type(metadata.get('code_bytes')) is not int or metadata['code_bytes'] != len(code):
        raise ValueError('Code size mismatch')
    words = list(struct.unpack('<' + str(len(code) // 4) + 'I', code))
    end = executable // 4
    if len(words) - end != 5 or any(w != 0xbf9f0000 for w in words[end:]):
        raise ValueError('Invalid five-DWORD ACO code-end padding')
    at, remaining, loads = 0, 0, 0
    clause_seen, waited, exported, ended = False, False, False, False
    offsets, pending, ready, listing = set(), {}, {}, []
    while at < end:
        start = at
        w = words[at]
        at += 1
        if w == 0xbf850003:  # SOPP s_clause, exactly four following VMEM loads.
            if clause_seen or start or remaining:
                raise ValueError('Duplicate/noninitial VMEM clause')
            clause_seen, remaining = True, 4
            text = 's_clause 3'
        elif (w & 0xfffff000) == 0xe0500000:  # GFX11 MUBUF buffer_load_dword.
            if not remaining or waited or exported or at >= end:
                raise ValueError('Load outside contiguous declared clause')
            second = words[at]
            at += 1
            offset, destination = w & 4095, second >> 8 & 255
            # Descriptor s0..3, inline-zero SOFFSET; OFFEN/IDXEN/TFE=0,
            # canonical unused VADDR128. No scalar writes are accepted anywhere.
            if second & 0xffff00ff != 0x80000080:
                raise ValueError('Load changed descriptor/address/cache contract')
            if offset not in (0, 4, 8, 12) or offset in offsets:
                raise ValueError('Load exceeds/duplicates the exact 16-byte resource')
            if destination >= metadata['num_vgprs'] or destination in pending:
                raise ValueError('Load overwrites pending result or exceeds allocation')
            pending[destination] = offset
            offsets.add(offset)
            remaining -= 1
            loads += 1
            text = f'buffer_load_dword v{destination}, s[0:3], 0 offset:{offset}'
        elif w == 0xbf8903f7:  # GFX11 VM[15:10]=0, LGKM[9:4]=63, EXP[2:0]=7.
            if remaining or loads != 4 or waited or exported:
                raise ValueError('Wait does not follow all four loads')
            ready, pending = pending, {}
            waited = True
            text = 's_waitcnt vmcnt(0)'
        elif w == 0xf800080f:  # GFX11 uncompressed full MRT0, DONE, no ROW_EN.
            if remaining or not waited or pending or exported or at >= end:
                raise ValueError('Export before real VMEM wait or duplicate export')
            second = words[at]
            at += 1
            indices = [(second >> (8 * i)) & 255 for i in range(4)]
            if any(ready.get(index) != component * 4 for component, index in enumerate(indices)):
                raise ValueError('Export does not preserve ready resource RGBA provenance')
            exported = True
            text = 'exp mrt0 ' + ', '.join('v' + str(v) for v in indices) + ' done'
        elif w == 0xbfb00000:
            if not exported or at != end:
                raise ValueError('Premature/nonfinal end')
            ended = True
            text = 's_endpgm'
        else:
            raise ValueError(f'Unsupported buffer PS instruction at {start * 4:#x}: {w:08x}')
        listing.append({'offset': start * 4, 'words': [f'{v:08x}' for v in words[start:at]], 'instruction': text})
    if not ended or not exported or pending or remaining or offsets != {0, 4, 8, 12}:
        raise ValueError('Incomplete buffer-read/export program')
    return {'success': True, 'code_sha256': hashlib.sha256(code).hexdigest(),
            'instructions': listing, 'buffer_read_offsets': sorted(offsets), 'descriptor_sgprs': [0, 1, 2, 3],
            'exported_resource_offsets': [0, 4, 8, 12], 'requires_native_16_byte_raw_descriptor': True,
            'gpu_executed': False, 'scope': 'offline machine-code decode, pending-load and component provenance proof'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('product', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('Existing evidence will not be overwritten')
    result = verify((args.product / 'shader.bin').read_bytes(), json.loads((args.product / 'shader.json').read_text()))
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'success': True, 'instructions': len(result['instructions'])}))


if __name__ == '__main__':
    main()
