#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded read-only Mach-O checks for the real x86_64 development bundle."""
import struct

MAX_BINARY = 64 * 1024 * 1024
GRAPHICS_EXPORTS = ['_hn_gfx11_fragment_buffer_compile_validated_file', '_hn_gfx11_fragment_buffer_descriptor',
                    '_hn_gfx11_fragment_compile_validated_file', '_hn_gfx11_ngg_compile_validated_file',
                    '_hn_gfx11_vertex_buffer_compile_validated_file', '_hn_gfx11_vertex_buffer_descriptor']
COLOR_EXPORTS = ['_hn_color_target_calculate', '_hn_color_target_validate', '_hn_color_target_validate_inputs']
GRAPHICS_LIBRARIES = ['libHackyNaviGraphicsCompiler.dylib', 'libHackyNaviColorLayout.dylib']


def macho(data):
    if not 32 <= len(data) <= MAX_BINARY:
        raise ValueError('Mach-O size outside bounds')
    magic, cpu, _, kind, count, total, _, _ = struct.unpack_from('<8I', data)
    if magic != 0xFEEDFACF or cpu != 0x01000007 or kind not in (2, 6, 8):
        raise ValueError('Expected actual thin x86_64 executable/dylib/bundle')
    if count > 4096 or total > len(data) - 32:
        raise ValueError('Invalid Mach-O load command bounds')
    cursor, end = 32, 32 + total
    result = {'file_type': kind, 'dependencies': [], 'rpaths': [], 'install_name': None,
              'exports': [], 'code_signature_present': False, 'minimum_os_raw': None,
              'platform': None, 'version_command': None, 'sdk_raw': None, 'build_tools': []}
    trie = None
    for _ in range(count):
        if cursor + 8 > end:
            raise ValueError('Truncated load command')
        cmd, size = struct.unpack_from('<II', data, cursor)
        if size < 8 or size % 8 or size > end - cursor:
            raise ValueError('Invalid load command size')
        # LC_LOAD_DYLIB, WEAK, REEXPORT, LAZY, UPWARD, ID and RPATH.
        if cmd in (0xC, 0x80000018, 0x8000001F, 0x20, 0x80000023, 0xD, 0x8000001C):
            minimum = 12 if cmd == 0x8000001C else 24
            if size < minimum:
                raise ValueError('Truncated path-bearing load command')
            offset = struct.unpack_from('<I', data, cursor + 8)[0]
            if not minimum <= offset < size:
                raise ValueError('Load command string outside command')
            stop = data.find(b'\0', cursor + offset, cursor + size)
            if stop < 0:
                raise ValueError('Unterminated load command string')
            path = data[cursor + offset:stop].decode('utf-8')
            if not path or any(part == '..' for part in path.split('/')) and cmd != 0x8000001C:
                raise ValueError('Empty or traversing dependency path')
            if cmd == 0xD:
                if result['install_name'] is not None:
                    raise ValueError('Duplicate install name')
                result['install_name'] = path
            else:
                result['rpaths' if cmd == 0x8000001C else 'dependencies'].append(path)
        elif cmd in (0x80000022, 0x80000033):
            minimum, offset = (48, 40) if cmd == 0x80000022 else (16, 8)
            if size < minimum or trie is not None:
                raise ValueError('Invalid/duplicate dynamic export trie')
            trie = struct.unpack_from('<II', data, cursor + offset)
        elif cmd == 0x1D:
            if size < 16:
                raise ValueError('Truncated code signature command')
            at, length = struct.unpack_from('<II', data, cursor + 8)
            if at > len(data) or length > len(data) - at:
                raise ValueError('Code signature outside binary')
            result['code_signature_present'] = bool(length)
        elif cmd in (0x32, 0x24, 0x25, 0x2F, 0x30):
            if result['version_command'] is not None:
                raise ValueError('Invalid OS-version command')
            # Apple's mach-o/loader.h defines a 24-byte build-version header
            # followed by exactly ntools 8-byte records. Legacy macOS uses one
            # fixed 16-byte version_min_command. Other platforms cannot supply
            # the native macOS target even when the CPU and file kind match.
            if cmd == 0x32:
                if size < 24:
                    raise ValueError('Truncated build-version command')
                platform, minimum_os, sdk, count_tools = struct.unpack_from('<4I', data, cursor + 8)
                if platform != 1 or count_tools != (size - 24) // 8:
                    raise ValueError('Non-macOS platform or invalid build-tool table')
                result['build_tools'] = [list(struct.unpack_from('<II', data, cursor + 24 + i * 8))
                                         for i in range(count_tools)]
            elif cmd == 0x24 and size == 16:
                platform = 1
                minimum_os, sdk = struct.unpack_from('<II', data, cursor + 8)
            else:
                raise ValueError('Unsupported platform or malformed legacy macOS version')
            result.update(platform=platform, version_command=cmd, minimum_os_raw=minimum_os, sdk_raw=sdk)
        cursor += size
    if cursor != end:
        raise ValueError('Load command count/size disagree')
    if trie:
        at, length = trie
        if at > len(data) or length > len(data) - at:
            raise ValueError('Export trie outside binary')
        if length:
            result['exports'] = exports(data[at:at + length])
    return result


def exports(data):
    names, visits = [], 0
    def uleb(at):
        value, shift = 0, 0
        while True:
            if at >= len(data) or shift >= 64:
                raise ValueError('Invalid export integer')
            byte = data[at]; at += 1; value |= (byte & 127) << shift
            if not byte & 128:
                return value, at
            shift += 7
    def visit(at, prefix, parents):
        nonlocal visits
        visits += 1
        if visits > 100000 or at in parents or len(parents) > 512 or len(prefix) > 16384:
            raise ValueError('Cyclic/excessive export trie')
        parents = parents | {at}
        terminal, at = uleb(at)
        end = at + terminal
        if end >= len(data):
            raise ValueError('Export node exceeds trie')
        if terminal:
            names.append(prefix)
        count, at = data[end], end + 1
        for _ in range(count):
            stop = data.find(b'\0', at)
            if stop < 0:
                raise ValueError('Unterminated export edge')
            suffix = data[at:stop].decode('utf-8')
            if not suffix:
                raise ValueError('Empty export edge')
            child, at = uleb(stop + 1)
            visit(child, prefix + suffix, parents)
    visit(0, '', set())
    if len(names) != len(set(names)):
        raise ValueError('Duplicate dynamic export')
    return sorted(names)


def validate_image(data, role, expected_exports=None, *, graphics=False):
    image = macho(data)
    expected_kind = {'frontend': 8, 'compiler': 6, 'graphics_compiler': 6, 'color_layout': 6, 'helper': 2}[role]
    if image['file_type'] != expected_kind:
        raise ValueError('Wrong native Mach-O type for ' + role)
    if image['platform'] != 1 or image['minimum_os_raw'] is None:
        raise ValueError('Binary lacks an explicit native macOS target')
    if not 0x000A0000 <= image['minimum_os_raw'] <= 0x000F0000:
        raise ValueError('Binary has an invalid or newer-than-macOS15.0 minimum target')
    wanted = '@rpath/libhackynavi_metal_compiler.dylib'
    required = [wanted] + (['@rpath/' + name for name in GRAPHICS_LIBRARIES] if graphics else [])
    for dep in image['dependencies']:
        if dep.startswith(('/usr/lib/', '/System/Library/Frameworks/')):
            continue
        if role == 'frontend' and dep in required:
            continue
        raise ValueError('Unpackaged/non-system dependency: ' + dep)
    expected_rpaths = ['@loader_path/../Frameworks'] if role == 'frontend' else []
    if image['rpaths'] != expected_rpaths:
        raise ValueError('Unexpected rpaths for ' + role)
    if role == 'frontend':
        if any(image['dependencies'].count(name) != 1 for name in required) or image['install_name'] is not None:
            raise ValueError('Bundle compiler dependency/identity mismatch')
        for name in ('_OBJC_CLASS_$_HNMetalDevice', '_OBJC_METACLASS_$_HNMetalDevice'):
            if name not in image['exports']:
                raise ValueError('Missing actual plugin class export: ' + name)
    elif role == 'compiler':
        if image['install_name'] != wanted:
            raise ValueError('Compiler install name mismatch')
        if expected_exports is None or image['exports'] != sorted(expected_exports):
            raise ValueError('Actual compiler export set differs from verified report')
    elif role in ('graphics_compiler', 'color_layout'):
        name, expected = (GRAPHICS_LIBRARIES[0], GRAPHICS_EXPORTS) if role == 'graphics_compiler' else (GRAPHICS_LIBRARIES[1], COLOR_EXPORTS)
        if image['install_name'] != '@rpath/' + name or image['exports'] != expected:
            raise ValueError('Graphics library identity/export contract mismatch')
    elif image['install_name'] is not None:
        raise ValueError('Helper has a dylib identity')
    return image
