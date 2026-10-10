#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Inspect actual linked Objective-C metadata without loading a Mach-O image.

Only thin x86_64 dylibs/bundles with supported dyld chained fixups are admitted.
Apple's fixup-chains.h and objc-runtime-new.h are pinned by content hash in
.research/objc-metadata/sources.json. This checks binary signatures, not GPU
execution, system discovery, arbitrary Objective-C semantics or bundle trust.
"""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path

MAX_IMAGE_BYTES = 64 * 1024 * 1024
MAX_CONTRACT_BYTES = 256 * 1024
MAX_IMPORTS = 16384
MAX_FIXUPS = 65536
MAX_CLASSES = 256
MAX_SYMBOLS = 262144
MAX_METHODS = 16384


class Invalid(ValueError):
    pass


class Image:
    def __init__(self, data):
        if len(data) > MAX_IMAGE_BYTES:
            raise Invalid('Image exceeds bounded metadata inspection size')
        self.data = data
        self.segments, self.sections, self.fixups, self.uuid = [], [], {}, None
        self.method_count = 0
        self.libraries, self.library_commands = [], []
        magic, cpu, _, kind, count, size, _, _ = self.unpack('<8I', 0)
        if (magic, cpu) != (0xfeedfacf, 0x1000007) or kind not in (6, 8):
            raise Invalid('Expected a thin x86_64 MH_DYLIB or MH_BUNDLE')
        if count > 256:
            raise Invalid('Excessive load commands')
        self.span(32, size)
        end, offset, chained, symbols = 32 + size, 32, None, None
        for _ in range(count):
            command, length = self.unpack('<II', offset)
            if length < 8 or length % 8 or offset + length > end:
                raise Invalid('Invalid load command extent')
            if command == 0x19:
                name = self.fixed(offset + 8, 16)
                vm, vm_size, file, file_size, _, protection, nsects, _ = self.unpack('<QQQQIIII', offset + 24)
                if length != 72 + nsects * 80 or file_size > vm_size:
                    raise Invalid('Invalid segment extent')
                self.span(file, file_size)
                self.segments.append(dict(name=name, vm=vm, size=vm_size, file=file, bytes=file_size, protection=protection))
                for index in range(nsects):
                    item = offset + 72 + index * 80
                    section = dict(name=self.fixed(item, 16), segment=self.fixed(item + 16, 16))
                    address, byte_count, file_offset = self.unpack('<QQI', item + 32)
                    if section['segment'] != name or address < vm or address + byte_count > vm + vm_size:
                        raise Invalid('Section outside segment')
                    section.update(vm=address, bytes=byte_count, file=file_offset)
                    self.sections.append(section)
            elif command in (0xc, 0x80000018, 0x8000001f, 0x20, 0x80000023):
                if length < 24:
                    raise Invalid('Invalid dylib import command')
                name_offset = self.unpack('<I', offset + 8)[0]
                if name_offset < 24 or name_offset >= length:
                    raise Invalid('Dylib import name outside command')
                self.libraries.append(self.cstring(offset + name_offset, offset + length))
                self.library_commands.append(command)
            elif command == 0x80000034:
                if chained is not None or length != 16:
                    raise Invalid('Duplicate/invalid chained fixups')
                chained = self.unpack('<II', offset + 8)
            elif command == 0x2:
                if symbols is not None or length != 24:
                    raise Invalid('Duplicate/invalid symbol table')
                symbols = self.unpack('<4I', offset + 8)
            elif command == 0x1b:
                if length != 24 or self.uuid:
                    raise Invalid('Duplicate/invalid UUID')
                self.uuid = self.span(offset + 8, 16).hex()
            offset += length
        if offset != end or not self.uuid or chained is None or symbols is None:
            raise Invalid('Missing required Mach-O metadata')
        bases = [s['vm'] for s in self.segments if s['name'] == '__TEXT' and s['file'] == 0]
        if len(bases) != 1:
            raise Invalid('Missing unique image base')
        self.base = bases[0]
        self.read_chains(*chained)
        self.symbols = self.read_symbols(*symbols)

    def span(self, offset, count):
        if offset < 0 or count < 0 or offset > len(self.data) or count > len(self.data) - offset:
            raise Invalid('Read outside image')
        return self.data[offset:offset + count]

    def unpack(self, format, offset):
        return struct.unpack(format, self.span(offset, struct.calcsize(format)))

    def fixed(self, offset, count):
        return self.span(offset, count).split(b'\0', 1)[0].decode('ascii')

    def cstring(self, offset, end=None):
        end = min(len(self.data), offset + 4096, len(self.data) if end is None else end)
        if offset < 0 or offset >= end:
            raise Invalid('Invalid string address')
        stop = self.data.find(b'\0', offset, end)
        if stop < 0:
            raise Invalid('Missing bounded string terminator')
        return self.data[offset:stop].decode('utf-8')

    def offset(self, address, count=1):
        matches = [s for s in self.segments if s['vm'] <= address and
                   address + count <= s['vm'] + s['bytes']]
        if len(matches) != 1:
            raise Invalid('Address is not in one file-backed segment')
        s = matches[0]
        return s['file'] + address - s['vm']

    def virtual(self, format, address):
        return self.unpack(format, self.offset(address, struct.calcsize(format)))

    def text(self, address):
        return self.cstring(self.offset(address))

    def pointer(self, address):
        if address in self.fixups:
            return self.fixups[address]
        if self.virtual('<Q', address)[0] == 0:
            return 0
        raise Invalid('Nonzero pointer has no supported chained fixup')

    def local(self, address):
        value = self.pointer(address)
        if not isinstance(value, int):
            raise Invalid('Expected local pointer, found external binding')
        return value

    def read_symbols(self, offset, count, strings, string_size):
        if count > MAX_SYMBOLS or count > len(self.data) // 16:
            raise Invalid('Excessive symbol count')
        self.span(offset, count * 16)
        self.span(strings, string_size)
        result = {}
        for index in range(count):
            name, kind, _, _, value = self.unpack('<IBBHQ', offset + index * 16)
            if name >= string_size:
                raise Invalid('Symbol name outside table')
            text = self.cstring(strings + name, strings + string_size)
            if kind & 0xe0 or not (kind & 0x0e):
                continue
            if not text.startswith('_OBJC_CLASS_$_'):
                continue
            if text in result and result[text] != value:
                raise Invalid('Conflicting symbol values')
            result[text] = value
        return result

    def read_chains(self, offset, length):
        self.span(offset, length)
        if length < 28:
            raise Invalid('Chained fixup header exceeds declared payload')
        version, starts, imports, strings, count, form, compression = self.unpack('<7I', offset)
        if version or compression or form not in (1, 2) or count > MAX_IMPORTS or count > length // 4:
            raise Invalid('Unsupported chained fixup header')
        for value in (starts, imports, strings):
            if value >= length:
                raise Invalid('Fixup subtable outside payload')
        stride = 4 if form == 1 else 8
        if imports + count * stride > length:
            raise Invalid('Import table outside payload')
        bindings = []
        for index in range(count):
            raw = self.unpack('<I', offset + imports + index * stride)[0]
            name_offset = raw >> 9
            if strings + name_offset >= length:
                raise Invalid('Import name outside payload')
            addend = self.unpack('<i', offset + imports + index * stride + 4)[0] if form == 2 else 0
            ordinal = raw & 0xff
            library = self.libraries[ordinal - 1] if 0 < ordinal <= len(self.libraries) else None
            load_command = self.library_commands[ordinal - 1] if 0 < ordinal <= len(self.libraries) else None
            bindings.append(dict(symbol=self.cstring(offset + strings + name_offset, offset + length),
                                 addend=addend, library=library, load_command=load_command,
                                 ordinal=ordinal, weak_import=bool((raw >> 8) & 1)))
        if starts + 4 > length:
            raise Invalid('Chained starts header exceeds declared payload')
        start = offset + starts
        segments = self.unpack('<I', start)[0]
        if segments != len(self.segments) or starts + 4 + segments * 4 > length:
            raise Invalid('Chained segment count mismatch')
        for index in range(segments):
            delta = self.unpack('<I', start + 4 + index * 4)[0]
            if not delta:
                continue
            if starts + delta + 22 > length:
                raise Invalid('Chained segment header outside payload')
            at = start + delta
            size, page_size, pointer_format, vm_offset, maximum, pages = self.unpack('<IHHQIH', at)
            if size < 22 + pages * 2 or starts + delta + size > length or page_size not in (4096, 16384):
                raise Invalid('Invalid chained segment extent')
            segment = self.segments[index]
            if pointer_format not in (2, 6) or maximum or self.base + vm_offset != segment['vm']:
                raise Invalid('Unsupported chained pointer format/segment')
            for page in range(pages):
                first = self.unpack('<H', at + 22 + page * 2)[0]
                if first == 0xffff:
                    continue
                if first & 0x8000:
                    raise Invalid('Multiple page chains are not admitted')
                page_base = segment['vm'] + page * page_size
                position = first
                while True:
                    if position + 8 > page_size:
                        raise Invalid('Chain leaves its page')
                    address = page_base + position
                    if position % 4 or address < segment['vm'] or address + 8 > segment['vm'] + segment['bytes']:
                        raise Invalid('Chain leaves its file-backed segment or stride')
                    if address in self.fixups:
                        raise Invalid('Duplicate/cyclic chain address')
                    if len(self.fixups) >= MAX_FIXUPS:
                        raise Invalid('Excessive chained fixup count')
                    raw = self.virtual('<Q', address)[0]
                    following = (raw >> 51) & 0xfff
                    if raw >> 63:
                        ordinal = raw & 0xffffff
                        if ordinal >= len(bindings) or ((raw >> 32) & 0x7ffff):
                            raise Invalid('Invalid chained binding')
                        entry = dict(bindings[ordinal]); entry['addend'] += (raw >> 24) & 0xff
                        self.fixups[address] = entry
                    else:
                        if (raw >> 44) & 0x7f:
                            raise Invalid('Reserved rebase bits are set')
                        target = (raw & ((1 << 36) - 1)) | (((raw >> 36) & 0xff) << 56)
                        self.fixups[address] = target + (self.base if pointer_format == 6 else 0)
                    if not following:
                        break
                    position += following * 4

    def methods(self, address):
        if not address:
            return {}
        flags, count = self.virtual('<II', address)
        small, direct = bool(flags & 0x80000000), bool(flags & 0x40000000)
        size = flags & 0xfffc
        if direct or flags & 0x3fff0000 or size != (12 if small else 24) or count > 4096:
            raise Invalid('Unsupported Objective-C method-list format')
        if count > MAX_METHODS - self.method_count:
            raise Invalid('Excessive total Objective-C method count')
        self.method_count += count
        self.offset(address, 8 + count * size)
        result = {}
        executable = [s for s in self.sections if s['segment'] == '__TEXT' and s['name'] == '__text']
        for index in range(count):
            at = address + 8 + index * size
            if small:
                name, types, imp = self.virtual('<iii', at)
                selector, encoding, implementation = self.local(at + name), at + 4 + types, at + 8 + imp
            else:
                selector, encoding, implementation = self.local(at), self.local(at + 8), self.local(at + 16)
            selector, encoding = self.text(selector), self.text(encoding)
            if selector in result or not any(s['vm'] <= implementation < s['vm'] + s['bytes'] for s in executable):
                raise Invalid('Duplicate selector or non-executable implementation')
            result[selector] = dict(encoding=encoding, implementation=implementation)
        return result

    def classes(self):
        result = {}
        self.method_count = 0
        lists = [s for s in self.sections if s['name'] == '__objc_classlist']
        if len(lists) != 1 or lists[0]['bytes'] % 8:
            raise Invalid('Expected one complete Objective-C class list')
        section = lists[0]
        if section['bytes'] // 8 > MAX_CLASSES:
            raise Invalid('Excessive Objective-C class count')
        if any(s['bytes'] for s in self.sections if s['name'] in ('__objc_catlist', '__objc_nlcatlist')):
            raise Invalid('Categories require separate override resolution')
        for index in range(section['bytes'] // 8):
            address = self.local(section['vm'] + index * 8)
            ro = self.local(address + 32) & ~7
            name = self.text(self.local(ro + 24))
            if name in result or self.symbols.get('_OBJC_CLASS_$_' + name) != address:
                raise Invalid('Class identity differs from actual symbol table')
            result[name] = dict(address=address, superclass=self.pointer(address + 8),
                                methods=self.methods(self.local(ro + 32)))
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--contract', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--class-name', default='HNMetalDevice')
    args = parser.parse_args()
    if args.output.exists():
        parser.error('Existing review evidence will not be overwritten')
    if args.image.stat().st_size > MAX_IMAGE_BYTES or args.contract.stat().st_size > MAX_CONTRACT_BYTES:
        parser.error('Input exceeds bounded metadata inspection size')
    with args.image.open('rb') as source:
        image_data = source.read(MAX_IMAGE_BYTES + 1)
    with args.contract.open('rb') as source:
        contract_data = source.read(MAX_CONTRACT_BYTES + 1)
    if len(image_data) > MAX_IMAGE_BYTES or len(contract_data) > MAX_CONTRACT_BYTES:
        parser.error('Read input exceeds bounded metadata inspection size')
    report = dict(success=False, image=str(args.image), image_sha256=hashlib.sha256(image_data).hexdigest(),
                  contract_sha256=hashlib.sha256(contract_data).hexdigest(),
                  checker_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  loaded_or_executed=False, class_name=args.class_name)
    try:
        entries = re.findall(r'\{"([^"\\]+)", "([^"\\]+)"\}', contract_data.decode())
        contract = dict(entries)
        if len(entries) != 392 or len(contract) != 392:
            raise Invalid('Expected the exact 392-method contract')
        image = Image(image_data)
        classes = image.classes()
        selected = classes.get(args.class_name)
        if selected is None:
            raise Invalid('Required real class is absent from compiled image')
        parent = selected['superclass']
        if (not isinstance(parent, dict) or parent['symbol'] != '_OBJC_CLASS_$__MTLDevice' or
                parent['addend'] or parent['weak_import'] or parent['load_command'] != 0xc or
                parent['library'] != '/System/Library/Frameworks/Metal.framework/Versions/A/Metal'):
            raise Invalid('Device is not linked to the genuine Metal superclass')
        methods = selected['methods']
        missing = sorted(set(contract) - set(methods))
        mismatches = {n:dict(expected=t, compiled=methods[n]['encoding']) for n,t in contract.items()
                      if n in methods and methods[n]['encoding'] != t}
        report.update(uuid=image.uuid, classes=sorted(classes), methods=methods, superclass=parent,
                      missing=missing, mismatches=mismatches, required_count=len(contract))
        if missing != ['_registerInterestNotification'] or mismatches:
            raise Invalid('Compiled device method metadata differs from the exact required contract')
        report['inherited_scope'] = 'Only _registerInterestNotification remains inherited; exact runtime validation checks the actual framework method before construction.'
        report['success'] = True
    except (Invalid, OSError, ValueError, struct.error) as error:
        report['error'] = str(error)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k:report.get(k) for k in ('success','error','required_count','missing','mismatches')}))
    return 0 if report['success'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
