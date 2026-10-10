#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Disassemble/check actual constant-color gfx1100 PS code; never execute it.

This deliberately bounded ISA verifier is for the offline fragment fixture
profile only, not a kernel security boundary or a software rendering backend.
Encodings: pinned Mesa aco_assembler.cpp:225,363,479,988; aco_opcodes.py:766,
796,1024,1495; aco_ir.h:604. GFX11 EXP has ROW_EN rather than old VM/COMPR bits.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def verify(code, metadata):
    if not code or len(code) > 1024 * 1024 or len(code) % 4:
        raise ValueError('Invalid code extent')
    if metadata.get('profile') != 'gfx1100-constant-rgba32-fragment-v1':
        raise ValueError('Wrong fragment profile')
    expected = {'abi_version': 1, 'target': 'gfx1100', 'stage': 'fragment', 'hardware_stage': 'pixel_shader',
                'mesa_commit': '35b085c4c067f198ce0b6437d94678e47b9c645f',
                'code_alignment': 256, 'wave_size': 32, 'input_vgprs': 1,
                'wgp_mode': False, 'spi_shader_pgm_rsrc2_ps': 0, 'spi_ps_input_ena': 128,
                'spi_ps_input_addr': 128, 'spi_shader_col_format': 9, 'cb_shader_mask': 15,
                'spi_ps_in_control': 32768, 'spi_shader_z_format': 0,
                'resource_count': 0, 'varying_count': 0, 'user_sgprs': 0,
                'scratch_bytes_per_wave': 0, 'lds_bytes': 0, 'constants_bytes': 0, 'relocations': [],
                'gpu_executed': False, 'draw_state_complete': False}
    for key, value in expected.items():
        if metadata.get(key) != value:
            raise ValueError('Incorrect fragment metadata: ' + key)
    for key in ('code_bytes', 'executable_bytes', 'num_sgprs', 'num_vgprs', 'float_mode'):
        if type(metadata.get(key)) is not int:
            raise ValueError('Noninteger fragment metadata: ' + key)
    executable = metadata['executable_bytes']
    vgprs = metadata['num_vgprs']
    if metadata['code_bytes'] != len(code) or not 0 < executable <= len(code) or executable % 4:
        raise ValueError('Inconsistent executable extent')
    if not 0 < vgprs <= 256 or not 0 <= metadata['num_sgprs'] <= 128 or not 0 <= metadata['float_mode'] <= 255:
        raise ValueError('Invalid register allocation')
    if type(metadata.get('mem_ordered')) is not bool:
        raise ValueError('Invalid memory ordering metadata')
    rsrc1 = (vgprs - 1) // 8 | 1 << 21 | metadata['float_mode'] << 12 | int(metadata['mem_ordered']) << 30
    if metadata.get('spi_shader_pgm_rsrc1_ps') != rsrc1:
        raise ValueError('PS resource register disagrees with allocation')
    color = metadata.get('color_bits')
    if not isinstance(color, list) or len(color) != 4 or any(type(v) is not int or not 0 <= v <= 0xffffffff for v in color):
        raise ValueError('Invalid color bit pattern')
    words = list(struct.unpack('<' + str(len(code) // 4) + 'I', code))
    end = executable // 4
    if any(word != 0xbf9f0000 for word in words[end:]) or len(words) - end != 5:
        raise ValueError('Noncanonical ACO five-word code-end padding')
    registers, listing = {}, []
    position = 0
    exported = None
    terminated = False

    def source(index, literal):
        if 128 <= index <= 192:
            return index - 128
        if 193 <= index <= 208:
            return (192 - index) & 0xffffffff
        inline = [0x3f000000, 0xbf000000, 0x3f800000, 0xbf800000,
                  0x40000000, 0xc0000000, 0x40800000, 0xc0800000, 0x3e22f983]
        if 240 <= index <= 248:
            return inline[index - 240]
        if index == 255 and literal is not None:
            return literal
        if 256 <= index <= 511 and index - 256 in registers:
            return registers[index - 256]
        raise ValueError('Undefined, scalar or unsupported MOV source')

    def put(index, value):
        if index >= vgprs:
            raise ValueError('Destination exceeds allocated VGPRs')
        registers[index] = value

    while position < end:
        start = position
        word = words[position]
        position += 1
        if word & 0xfe01fe00 == 0x7e000200:  # VOP1 v_mov_b32
            if exported is not None:
                raise ValueError('MOV after final export')
            src, dest = word & 511, word >> 17 & 255
            literal = None
            if src == 255:
                if position >= end:
                    raise ValueError('Truncated MOV literal')
                literal = words[position]; position += 1
            value = source(src, literal); put(dest, value)
            text = f'v_mov_b32 v{dest}, 0x{value:08x}'
        elif word & 0xfffe0000 == 0xca100000:  # VOPD two actual v_dual_mov_b32
            if exported is not None or word & 0x1fe00 or position >= end:
                raise ValueError('Invalid/truncated VOPD MOV pair')
            second = words[position]; position += 1
            if second & 0x1fe00:
                raise ValueError('Reserved VOPD MOV operands')
            sx, sy = word & 511, second & 511
            dx = second >> 24
            dy = (second >> 17 & 127) * 2 + (1 - (dx & 1))
            literal = None
            if sx == 255 or sy == 255:
                if position >= end:
                    raise ValueError('Truncated VOPD literal')
                literal = words[position]; position += 1
            vx, vy = source(sx, literal), source(sy, literal)
            put(dx, vx); put(dy, vy)
            text = f'v_dual_mov_b32 v{dx}, 0x{vx:08x} :: v_dual_mov_b32 v{dy}, 0x{vy:08x}'
        elif word == 0xf800080f:
            if exported is not None or position >= end:
                raise ValueError('Duplicate/truncated export')
            operands = words[position]; position += 1
            indices = [(operands >> (8 * i)) & 255 for i in range(4)]
            if any(v not in registers for v in indices):
                raise ValueError('Export reads uninitialized system or unknown VGPR')
            exported = [registers[v] for v in indices]
            text = 'exp mrt0 ' + ', '.join('v' + str(v) for v in indices) + ' done'
        elif word == 0xbfb00000:
            if exported is None or position != end:
                raise ValueError('Premature or nonfinal s_endpgm')
            terminated = True
            text = 's_endpgm'
        else:
            raise ValueError(f'Instruction outside constant PS verification profile at {start * 4:#x}: {word:08x}')
        listing.append({'offset': start * 4, 'words': [f'{v:08x}' for v in words[start:position]], 'instruction': text})
    if not terminated or exported != color:
        raise ValueError('Actual machine export differs from declared source color')
    return {'success': True, 'code_sha256': hashlib.sha256(code).hexdigest(),
            'exported_constant_bits': exported, 'instructions': listing,
            'gpu_executed': False, 'scope': 'bounded actual machine-code disassembly and constant export proof; no shader execution'}


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
