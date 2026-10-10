#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Opcode metadata/encoding fields derived from Mesa; Copyright (c) 2018 Valve Corporation.
"""Generate research-only GFX1100 rows from pinned Mesa and real ACO products.

This is a bounded structural inspector, NOT shader admission: no register
initialization, pending-load/provenance proof, resource ownership or GPU run.
Unknown encodings fail. No generated row constitutes permission to execute.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
MESA = '35b085c4c067f198ce0b6437d94678e47b9c645f'
SOURCES = {
    'aco_opcodes.py': '18556f741f8818bae953eff4bb602bff5f57aceb98ac918eb13d817ac3294e3e',
    'aco_assembler.cpp': '1a5cdbb01a6f850aed1cd5196c23340c8dc01ddb6954e6bcb29e5eaa36bd8935',
    'aco_opcodes_cpp.py': 'dde16a9fbb0ab780f24ac4ab75504412249e4fa526b7d9f2ef4e8a0f61d57c16',
    'aco_ir.h': '8cf0be514b44957a1c3890b21a5b99e59fc2a5800f8d225a2f9fc33fcc67d40d',
    'aco_ir.cpp': 'dca95f27e90bce78dd6ee8f104bbb0fde5590165b245ae3b371109b9760163cb',
}
NAMES = ('s_mov_b32', 's_mul_i32', 's_clause', 's_waitcnt', 's_delay_alu',
         's_endpgm', 's_code_end', 's_load_dword', 's_load_dwordx4',
         'v_lshl_add_u32', 'v_add_lshl_u32', 'v_add3_u32', 'v_mul_lo_u32',
         'v_xad_u32', 'v_lshlrev_b32', 'v_add_u32',
         'buffer_load_dword', 'buffer_store_dword')
DENY = ('s_setpc_b64', 's_swappc_b64', 's_rfe_b64', 's_sendmsg_rtn_b32',
        's_sendmsg_rtn_b64', 's_getreg_b32', 's_setreg_b32', 's_setreg_imm32_b32',
        's_trap', 's_sendmsg', 's_sendmsghalt', 's_endpgm_saved')
EXPECTED = {
    ('add_one', 64): 'ac333acf99387a4063fae75a89dd4e6968907f6b78702d26442dd8a3575f7dda',
    ('add_one', 3): '98b773c473a79de6d64f7ed6f9e559fd88d5a87058e67dfab484bd1264a5c93f',
    ('affine_xor', 64): '66bc66e2cb814db18d426cc38eb52bc4c843b3a1619952de20945d9a018ce0f4',
    ('affine_xor', 3): '0a9ad10b394dce951c53a04726ff40a2f24dee3574758a905382ed7f79b0a723',
}


class DecodeError(ValueError):
    pass


def field(name, word, low, bits, role, scale=1):
    return dict(name=name, word=word, low=low, bits=bits, role=role, scale=scale)


def formats():
    # The opcode width here is the available encoding container used to reject
    # every unlisted value. In particular, no high/reserved opcode bit is
    # ignored. SMEM supports only Mesa's nonnegative20-bit immediate subset.
    return {
        'SOP1': dict(mask=0xff800000, value=0xbe800000, shift=8, bits=8, words=1,
            source='aco_assembler.cpp:197-207', fields=[field('sdst',0,16,7,'sgpr32_destination'),field('src0',0,0,8,'scalar_source_selector')]),
        'SOP2': dict(mask=0xc0000000, value=0x80000000, shift=23, bits=7, words=1,
            source='aco_assembler.cpp:151-161', fields=[field('sdst',0,16,7,'sgpr32_destination'),field('src0',0,0,8,'scalar_source_selector'),field('src1',0,8,8,'scalar_source_selector')]),
        'SOPP': dict(mask=0xff800000, value=0xbf800000, shift=16, bits=7, words=1,
            source='aco_assembler.cpp:221-237', fields=[field('simm16',0,0,16,'opcode_specific_immediate')]),
        'SMEM': dict(mask=0xfc000000, value=0xf4000000, shift=18, bits=8, words=2,
            source='aco_assembler.cpp:240-343; aco_ir.cpp:145-149; aco_ir.h:1570-1578', fields=[
                field('sbase',0,0,6,'sgpr64_base',2),field('sdata',0,6,7,'sgpr_load_destination'),
                field('dlc',0,13,1,'cache_flag'),field('glc',0,14,1,'cache_flag'),
                field('offset',1,0,20,'nonnegative_byte_offset_subset'),field('soffset',1,25,7,'sgpr32_offset_or_gfx11_null')]),
        'MUBUF': dict(mask=0xfc000000, value=0xe0000000, shift=18, bits=8, words=2,
            source='aco_assembler.cpp:554-603', fields=[field('offset',0,0,12,'byte_offset'),
                field('slc',0,12,1,'cache_flag'),field('dlc',0,13,1,'cache_flag'),field('glc',0,14,1,'cache_flag'),
                field('lds',0,16,1,'lds_destination'),field('vaddr',1,0,8,'vgpr32_address'),
                field('vdata',1,8,8,'vgpr32_load_destination_or_store_source'),field('srsrc',1,16,5,'sgpr128_descriptor',4),
                field('tfe',1,21,1,'texture_fail_enable'),field('offen',1,22,1,'vector_offset_enable'),
                field('idxen',1,23,1,'vector_index_enable'),field('soffset',1,24,8,'scalar_source_selector')]),
        'VOP3': dict(mask=0xfc000000, value=0xd4000000, shift=16, bits=10, words=2,
            source='aco_assembler.cpp:1074-1140; integer no-modifier subset only', fields=[
                field('vdst',0,0,8,'vgpr32_destination'),field('src0',1,0,9,'scalar_or_vector_source_selector'),
                field('src1',1,9,9,'scalar_or_vector_source_selector'),field('src2',1,18,9,'scalar_or_vector_source_selector')]),
        'VOP2': dict(mask=0x80000000, value=0, shift=25, bits=6, words=1,
            source='aco_assembler.cpp:346-361; integer no-extension subset only', fields=[
                field('vdst',0,17,8,'vgpr32_destination'),field('src0',0,0,9,'scalar_or_vector_source_selector'),
                field('vsrc1',0,9,8,'vgpr32_source')]),
    }


def mesa_module(directory):
    for name, expected in SOURCES.items():
        raw = (directory / name).read_bytes().replace(b'\r\n', b'\n')
        if hashlib.sha256(raw).hexdigest() != expected:
            raise ValueError('pinned Mesa source mismatch: ' + name)
    spec = importlib.util.spec_from_file_location('pinned_aco_opcodes', directory / 'aco_opcodes.py')
    module = importlib.util.module_from_spec(spec)
    previous = sys.dont_write_bytecode
    try:
        sys.dont_write_bytecode = True
        spec.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = previous
    return module


def type_record(value):
    return dict(base=value.base_type.name, bits=value.bit_size, components=value.num_components,
                fixed=value.fixed_reg.name, modifiers=value.mods)


def rows(module):
    result = []
    for name in NAMES:
        instruction = module.instructions[name]
        form = instruction.format.name
        layout = formats()[form]
        opcode = instruction.op.gfx11
        if opcode < 0 or opcode >= 1 << layout['bits']:
            raise ValueError('opcode does not fit reviewed format: ' + name)
        aliases = sorted(n for n,i in module.instructions.items()
                         if i.format == instruction.format and i.op.gfx11 == opcode and n != name)
        if aliases and not (name == 'v_mul_lo_u32' and aliases == ['v_mul_lo_i32']):
            raise ValueError('unreviewed opcode alias: ' + name)
        row = dict(name=name, aliases=aliases, format=form, opcode=opcode,
            match_mask=layout['mask'] | (((1 << layout['bits']) - 1) << layout['shift']),
            match_value=layout['value'] | (opcode << layout['shift']),
            instruction_class=instruction.cls.value, atomic=instruction.is_atomic == '1',
            operands=[type_record(v) for v in instruction.operands],
            definitions=[type_record(v) for v in instruction.definitions],
            operand_metadata_complete=bool(instruction.operands or instruction.definitions),
            admission=False, padding_only=name == 's_code_end')
        if form in ('SMEM','MUBUF'):
            row['operand_metadata_note'] = 'Mesa opcode table has no operand types for this format; physical fields/explicit widths below are sourced from assembler/IR, not invented table metadata.'
        if name.startswith('s_load_'):
            row['memory_bytes'] = 16 if name.endswith('x4') else 4
            row['pending_counter'] = 'lgkm'
        if form == 'MUBUF':
            row['memory_bytes'] = 4
            row['memory_direction'] = 'read' if name == 'buffer_load_dword' else 'write'
            row['pending_counter'] = 'vm' if name == 'buffer_load_dword' else 'vs'
        result.append(row)
    return result


def source_selector(value, vector):
    if 0 <= value <= 105:
        return 'sgpr'
    if 128 <= value <= 192:
        return 'inline_nonnegative_integer'
    if 193 <= value <= 208:
        return 'inline_negative_integer'
    if value == 255:
        return 'literal32'
    if vector and 256 <= value <= 511:
        return 'vgpr'
    raise DecodeError('unreviewed special/extended source selector: ' + str(value))


def decode(code, executable_bytes, table):
    if not code or len(code) > 1024 * 1024 or len(code) % 4 or executable_bytes % 4 or not 0 < executable_bytes <= len(code):
        raise DecodeError('invalid executable extent')
    words = struct.unpack('<' + 'I' * (len(code) // 4), code)
    output, cursor = [], 0
    while cursor < len(words):
        candidates = [r for r in table if words[cursor] & r['match_mask'] == r['match_value']]
        if len(candidates) != 1:
            raise DecodeError(f'unknown/ambiguous instruction at byte {cursor*4}')
        row = candidates[0]
        form, name = row['format'], row['name']
        layout = formats()[form]
        count = layout['words']
        if cursor + count > len(words):
            raise DecodeError('truncated multiword instruction')
        raw = list(words[cursor:cursor+count])
        allowed = [0] * count
        allowed[0] = row['match_mask']
        values = {}
        for f in layout['fields']:
            mask = ((1 << f['bits']) - 1) << f['low']
            allowed[f['word']] |= mask
            values[f['name']] = ((raw[f['word']] & mask) >> f['low']) * f['scale']
        if any(w & ~mask for w,mask in zip(raw,allowed)):
            raise DecodeError('unreviewed reserved/modifier/offset bits')
        literal = False
        for f in layout['fields']:
            if f['role'] in ('scalar_source_selector','scalar_or_vector_source_selector'):
                kind = source_selector(values[f['name']], f['role'] == 'scalar_or_vector_source_selector')
                literal |= kind == 'literal32'
        if form in ('SOP1','SOP2') and values['sdst'] > 105:
            raise DecodeError('special SGPR destination')
        if form == 'SMEM':
            width = row['memory_bytes'] // 4
            if values['sbase'] + 1 > 105 or values['sdata'] + width - 1 > 105 or values['soffset'] not in (*range(106),124):
                raise DecodeError('special SMEM register')
            if values['glc'] or values['dlc']:
                raise DecodeError('unreviewed SMEM cache policy')
        if form == 'MUBUF':
            if values['srsrc'] + 3 > 105 or any(values[f] for f in ('slc','dlc','glc','lds','tfe','idxen')) or values['offen'] != 1:
                raise DecodeError('unreviewed MUBUF address/cache policy')
            if literal:
                raise DecodeError('MUBUF literal offset not implemented')
        if form == 'VOP3' and len(row['operands']) == 2 and values['src2'] != 128:
            raise DecodeError('nonzero unused VOP3 operand')
        if literal:
            if cursor + count >= len(words):
                raise DecodeError('truncated literal')
            raw.append(words[cursor+count]);count += 1
            values['literal32'] = raw[-1]
        padding = cursor * 4 >= executable_bytes
        if padding != row['padding_only'] or (not padding and (cursor+count)*4 > executable_bytes):
            raise DecodeError('instruction/padding crosses executable extent')
        if name == 's_endpgm' and ((cursor+count)*4 != executable_bytes or values['simm16']):
            raise DecodeError('nonterminal or modified endpgm')
        if name == 's_code_end' and values['simm16']:
            raise DecodeError('modified padding')
        if name == 's_waitcnt':
            imm = values['simm16']
            if imm & 8:
                raise DecodeError('reserved waitcnt bit')
            values['vmcnt'] = (imm >> 10) & 63
            values['lgkmcnt'] = (imm >> 4) & 63
            values['expcnt'] = imm & 7
        output.append(dict(byte_offset=cursor*4, name=name, format=form, dwords=count,
                           words=[f'{w:08x}' for w in raw], fields=values, padding=padding))
        cursor += count
    executable = [i for i in output if not i['padding']]
    if not executable or executable[-1]['name'] != 's_endpgm':
        raise DecodeError('missing terminal endpgm')
    return output


def generate(mesa, products):
    module = mesa_module(mesa)
    table = rows(module)
    corpus = json.loads(products.read_bytes())
    if corpus.get('schema') != 1 or corpus.get('hardware_executed') is not False or corpus.get('native_admission') is not False:
        raise ValueError('unexpected corpus schema/claim')
    observed, seen = [], set()
    for product in corpus['variants']:
        if product['local'] not in ([64,1,1],[3,1,1]) or product.get('standalone_equal') is not True:
            raise ValueError('unexpected variant/capture contract')
        key = (product['kernel'], product['local'][0])
        code = bytes.fromhex(product['code_hex'])
        if key not in EXPECTED or key in seen or hashlib.sha256(code).hexdigest() != EXPECTED[key]:
            raise ValueError('unexpected or changed real compiler product')
        metadata = product['metadata']
        if product['code_sha256'] != EXPECTED[key] or metadata['target'] != 'gfx1100' or metadata['mesa_commit'] != MESA or metadata['code_bytes'] != len(code) or metadata['workgroup_size'] != product['local']:
            raise ValueError('corpus metadata mismatch')
        seen.add(key)
        decoded = decode(code, product['metadata']['executable_bytes'], table)
        observed.append(dict(kernel=key[0], local=product['local'], code_sha256=EXPECTED[key],
            code_bytes=len(code), executable_bytes=product['metadata']['executable_bytes'],
            instructions=decoded))
    if seen != set(EXPECTED):
        raise ValueError('missing required real compiler variants')
    used = {i['name'] for p in observed for i in p['instructions']}
    if used != set(NAMES):
        raise ValueError('reviewed row set no longer exactly matches observed products')
    forbidden = [dict(name=n, format=module.instructions[n].format.name, opcode=module.instructions[n].op.gfx11)
                 for n in DENY]
    return dict(schema=1, target='gfx1100', mesa_commit=MESA, hardware_executed=False,
        native_admission=False, default_policy='reject unknown/ambiguous/unimplemented encoding',
        claim='Source-derived structural research rows only; not a provenance, initialization, memory-ordering, termination or ownership proof.',
        source_sha256_lf=SOURCES, corpus_sha256=hashlib.sha256(products.read_bytes()).hexdigest(),
        formats=formats(), rows=table, explicit_reject_examples=forbidden, products=observed,
        limits=['No independent LLVM disassembler result: available Windows LLVM20 has no AMDGPU target.',
                'Only the recorded integer/no-modifier forms and nonnegative20-bit SMEM offset subset are inspected.',
                'SMEM/MUBUF opcode metadata lacks operand types; separate physical-field widths are supplied explicitly.',
                'The caller-provided executable length is checked structurally here; it is not native authority.',
                'No generated row or successful structural decode authorizes shader execution.'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mesa', type=Path, default=ROOT / '.research/mesa/src/amd/compiler')
    parser.add_argument('--products', type=Path, default=HERE / 'fixtures/compute-products/products.json')
    parser.add_argument('--output', type=Path, default=HERE / 'generated/gfx1100-leaf-decoder-rows.json')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    data = json.dumps(generate(args.mesa, args.products), indent=2) + '\n'
    if args.check:
        if args.output.read_text(encoding='utf-8') != data:
            raise SystemExit('generated decoder artifact is stale')
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(data, encoding='utf-8')
    print('18 source-derived rows; 4 real compiler variants; no native admission')


if __name__ == '__main__':
    main()
