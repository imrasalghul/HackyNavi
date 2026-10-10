#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded inspection of the actual thin Darwin objects used by graphics builds."""
import struct
import hashlib


def macho(data, filetype):
    if len(data) < 32 or data[:4] != b'\xcf\xfa\xed\xfe':
        raise ValueError('Expected thin little-endian Mach-O64')
    _, cpu, _, kind, count, commands, _, reserved = struct.unpack_from('<8I', data)
    if cpu != 0x1000007 or kind != filetype or reserved or count > 4096 or 32 + commands > len(data):
        raise ValueError('Unexpected Mach-O target/header')
    offset = 32
    platforms, dylibs, rpaths, identifiers = [], [], [], []
    for _ in range(count):
        if offset + 8 > 32 + commands:
            raise ValueError('Truncated Mach-O command')
        command, size = struct.unpack_from('<2I', data, offset)
        if size < 8 or size % 8 or offset + size > 32 + commands:
            raise ValueError('Invalid Mach-O command size')
        if command == 0x32:
            if size < 24:
                raise ValueError('Short build-version command')
            platform, minimum, sdk, tools = struct.unpack_from('<4I', data, offset + 8)
            if platform != 1 or minimum != 0xf0000 or size != 24 + tools * 8:
                raise ValueError('Expected macOS15.0 build target')
            platforms.append({'platform': platform, 'minimum': minimum, 'sdk': sdk})
        elif command in (0x24, 0x25, 0x2f, 0x30):
            raise ValueError('Unexpected legacy OS target')
        elif command in (0xc, 0xd, 0x80000018, 0x8000001f, 0x8000001c):
            minimum_size = 12 if command == 0x8000001c else 24
            if size < minimum_size:
                raise ValueError('Short dylib/path command')
            start, = struct.unpack_from('<I', data, offset + 8)
            if start < minimum_size or start >= size:
                raise ValueError('Invalid dylib/path string offset')
            raw = data[offset + start:offset + size]
            if b'\0' not in raw:
                raise ValueError('Unterminated dylib/path')
            name = raw.split(b'\0', 1)[0].decode('utf-8')
            (rpaths if command == 0x8000001c else identifiers if command == 0xd else dylibs).append(name)
        offset += size
    if offset != 32 + commands or len(platforms) != 1:
        raise ValueError('Missing/duplicate OS target or command size mismatch')
    return {'filetype': kind, 'architecture': 'x86_64', 'platforms': platforms,
            'dependencies': dylibs, 'rpaths': rpaths, 'identifiers': identifiers}


def archive(data):
    if not data.startswith(b'!<arch>\n') or len(data) > 128 * 1024 * 1024:
        raise ValueError('Invalid archive')
    offset, objects = 8, []
    while offset < len(data):
        header = data[offset:offset + 60]
        if len(header) != 60 or header[58:] != b'`\n':
            raise ValueError('Invalid archive member header')
        size = int(header[48:58])
        name = header[:16].decode('ascii').strip()
        offset += 60
        if size < 0 or offset + size > len(data):
            raise ValueError('Invalid archive member length')
        payload = data[offset:offset + size]
        offset += size + (size % 2)
        if name.startswith('#1/'):
            length = int(name[3:])
            if length > len(payload):
                raise ValueError('Invalid extended archive member name')
            name = payload[:length].rstrip(b'\0').decode('utf-8')
            payload = payload[length:]
        if name.startswith('__.SYMDEF') or name in ('/', '//', '/SYM64/'):
            continue
        info = macho(payload, 1)
        objects.append({'name': name, 'bytes': len(payload), 'sha256': hashlib.sha256(payload).hexdigest(),
                        'target': info['platforms'][0]})
        if len(objects) > 4096:
            raise ValueError('Archive member limit')
    if offset != len(data) or not objects:
        raise ValueError('Archive framing or empty archive')
    return objects
