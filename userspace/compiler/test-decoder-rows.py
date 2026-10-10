#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Structural regression tests; passing them is not native shader admission."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('decoder_rows', HERE / 'generate-gfx11-decoder-rows.py')
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


class DecoderRows(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mesa = generator.ROOT / '.research/mesa/src/amd/compiler'
        cls.corpus_path = HERE / 'fixtures/compute-products/products.json'
        cls.products = json.loads(cls.corpus_path.read_bytes())['variants']
        cls.table = generator.rows(generator.mesa_module(cls.mesa))

    def inspect(self, product, code=None, executable=None):
        return generator.decode(code if code is not None else bytes.fromhex(product['code_hex']),
            executable if executable is not None else product['metadata']['executable_bytes'], self.table)

    def patched(self, product, offset, value):
        data = bytearray.fromhex(product['code_hex'])
        struct.pack_into('<I', data, offset, value)
        return bytes(data)

    def test_four_actual_variants_exact_word_roundtrip(self):
        for product in self.products:
            with self.subTest(kernel=product['kernel'], local=product['local']):
                decoded = self.inspect(product)
                words = [int(word,16) for i in decoded for word in i['words']]
                self.assertEqual(struct.pack('<'+'I'*len(words),*words).hex(), product['code_hex'])
                self.assertEqual(sum(i['dwords'] for i in decoded)*4, len(bytes.fromhex(product['code_hex'])))
                self.assertEqual([i['name'] for i in decoded if not i['padding']][-1], 's_endpgm')

    def test_every_executable_truncation_and_wrong_boundary(self):
        for product in self.products:
            code = bytes.fromhex(product['code_hex'])
            executable = product['metadata']['executable_bytes']
            for end in range(executable):
                with self.subTest(kernel=product['kernel'], local=product['local'], end=end):
                    with self.assertRaises(generator.DecodeError):
                        self.inspect(product,code[:end])
            for end in (executable-4, executable+4, 1, len(code)+4):
                with self.assertRaises(generator.DecodeError):
                    self.inspect(product,executable=end)

    def test_unknown_privileged_and_unsupported_formats_rejected(self):
        product = self.products[0]
        # trap, sendmsg, setreg, setpc, GLOBAL/FLAT, DS, unlisted SOPP, VOP1.
        for word in (0xbf900000,0xbfb60000,0xb9000000,0xbe804800,
                     0xdc520000,0xd8000000,0xbfff0000,0x7e000280):
            with self.subTest(word=hex(word)), self.assertRaises(generator.DecodeError):
                self.inspect(product,self.patched(product,0,word))

    def test_literal_payload_is_data_not_opcode(self):
        product = next(p for p in self.products if p['kernel']=='affine_xor')
        instruction = next(i for i in self.inspect(product) if i['name']=='v_xad_u32')
        self.assertEqual(instruction['dwords'],3)
        offset = instruction['byte_offset']+8
        decoded = self.inspect(product,self.patched(product,offset,0xbf900000))
        same = next(i for i in decoded if i['byte_offset']==instruction['byte_offset'])
        self.assertEqual(same['fields']['literal32'],0xbf900000)
        self.assertEqual(same['name'],'v_xad_u32')

    def test_special_registers_flags_and_modifiers_rejected(self):
        product=self.products[0]
        decoded=self.inspect(product)
        smem=next(i for i in decoded if i['name']=='s_load_dwordx4')
        buffer=next(i for i in decoded if i['name']=='buffer_load_dword')
        valu=next(i for i in decoded if i['name']=='v_lshl_add_u32')
        mutations=[(0,0xbef00090), # s_mov into trap temporary112
            (smem['byte_offset'],(int(smem['words'][0],16)&~(127<<6))|(105<<6)),
            (smem['byte_offset']+4,112<<25),
            (smem['byte_offset']+4,1<<20), # outside reviewed positive20-bit subset
            (buffer['byte_offset'],int(buffer['words'][0],16)|(1<<16)),
            (buffer['byte_offset']+4,(int(buffer['words'][1],16)&~(31<<16))|(27<<16)),
            (valu['byte_offset'],int(valu['words'][0],16)|(1<<8)),
            (valu['byte_offset']+4,(int(valu['words'][1],16)&~511)|249)]
        for offset,word in mutations:
            with self.subTest(offset=offset, word=hex(word)), self.assertRaises(generator.DecodeError):
                self.inspect(product,self.patched(product,offset,word))

    def test_waitcnt_uses_gfx11_bit_positions(self):
        values=[i['fields'] for i in self.inspect(self.products[0]) if i['name']=='s_waitcnt']
        self.assertEqual([(v['vmcnt'],v['lgkmcnt'],v['expcnt']) for v in values],[(63,0,7),(0,63,7)])

    def test_structural_success_does_not_claim_provenance(self):
        product=self.products[0]
        buffer=next(i for i in self.inspect(product) if i['name']=='buffer_load_dword')
        # Structurally valid quartet s[16:19], which this program never initialized.
        # This inspector MUST NOT be used as a security/provenance verifier.
        word=(int(buffer['words'][1],16)&~(31<<16))|(4<<16)
        self.assertTrue(self.inspect(product,self.patched(product,buffer['byte_offset']+4,word)))
        self.assertTrue(all(not row['admission'] for row in self.table))

    def test_generation_is_exact_and_changed_corpus_rejected(self):
        result=generator.generate(self.mesa,self.corpus_path)
        self.assertEqual(len(result['rows']),18)
        self.assertFalse(result['native_admission'])
        self.assertEqual(result,json.loads((HERE/'generated/gfx1100-leaf-decoder-rows.json').read_bytes()))
        corpus=json.loads(self.corpus_path.read_bytes())
        corpus['variants'][0]['code_hex']='00'+corpus['variants'][0]['code_hex'][2:]
        with tempfile.TemporaryDirectory(prefix='hn-decoder-negative-') as temporary:
            path=Path(temporary)/'products.json';path.write_text(json.dumps(corpus))
            with self.assertRaises(ValueError):generator.generate(self.mesa,path)


if __name__ == '__main__':
    unittest.main(verbosity=2)
