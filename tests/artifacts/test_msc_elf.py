#!/usr/bin/env python3
"""Test final ARM ELF driver pointers using symbols and DWARF-derived layout.

Requires unstripped ELF with DWARF and arm-none-eabi nm/readelf. No builds run.
Paths and SHA-256 identify artifacts, not their transport provenance.
"""
import hashlib
import json
import re
import shutil
import struct
import subprocess
from pathlib import Path

import pytest

pytestmark = pytest.mark.artifacts


def require(condition, message):
    if not condition:
        raise ValueError(message)


def driver_layout(debug):
    dies = {}
    for match in re.finditer(r'<(\d+)><([0-9a-f]+)>:[^\n]*?\((DW_TAG_\w+)\)(.*?)(?=\n\s*<\d+><|\Z)', debug, re.S):
        depth, address, tag, body = match.groups()
        attrs = dict(re.findall(r'DW_AT_(\w+)\s*:\s*([^\n]+)', body))
        dies[int(address, 16)] = (int(depth), tag, attrs)
    layouts = set()
    for _, tag, attrs in dies.values():
        if tag != 'DW_TAG_typedef' or attrs.get('name', '').split(':')[-1].strip() != 'usbd_class_driver_t':
            continue
        ref = int(re.fullmatch(r'<0x([0-9a-f]+)>', attrs['type']).group(1), 16)
        depth, kind, fields = dies[ref]
        require(kind == 'DW_TAG_structure_type', 'driver typedef is not a structure')
        size = int(fields['byte_size'])
        members = {}
        active = False
        for address, (child_depth, child_tag, child_attrs) in dies.items():
            if address == ref:
                active = True
                continue
            if not active:
                continue
            if child_depth <= depth:
                break
            if child_depth == depth + 1 and child_tag == 'DW_TAG_member':
                name = child_attrs['name'].split(':')[-1].strip()
                if name in ('open', 'reset', 'control_xfer_cb'):
                    pointer_ref = int(re.fullmatch(r'<0x([0-9a-f]+)>', child_attrs['type']).group(1), 16)
                    _, pointer_kind, pointer_attrs = dies[pointer_ref]
                    require(pointer_kind == 'DW_TAG_pointer_type' and int(pointer_attrs['byte_size']) == 4,
                            'expected ARM 32-bit function pointer')
                    members[name] = int(child_attrs['data_member_location'])
        require(set(members) == {'open', 'reset', 'control_xfer_cb'}, 'missing driver members')
        require(all(0 <= offset <= size - 4 and offset % 4 == 0 for offset in members.values()), 'invalid member offsets')
        require(len(set(members.values())) == 3, 'overlapping members')
        layouts.add((size, members['open'], members['reset'], members['control_xfer_cb']))
    require(len(layouts) == 1, 'missing or conflicting DWARF driver layout')
    return layouts.pop()


def check_table(table, layout, symbols):
    size, open_offset, reset_offset, control_offset = layout
    require(size > 0 and len(table) > 0 and len(table) % size == 0, 'invalid driver table size')
    # ARM Thumb function pointers must include the state bit; nm may omit it.
    pointer = lambda name: symbols[name][0] | 1
    entries = [i for i in range(0, len(table), size)
               if struct.unpack_from('<I', table, i + open_offset)[0] == pointer('mscd_open')]
    require(len(entries) == 1, 'expected exactly one MSC driver identified by mscd_open')
    for offset, name in ((reset_offset, '__wrap_mscd_reset'),
                         (control_offset, '__wrap_mscd_control_xfer_cb')):
        require(struct.unpack_from('<I', table, entries[0] + offset)[0] == pointer(name),
                f'MSC driver pointer does not resolve to {name}')
    return entries[0]


def check_elf(path):
    path = Path(path).resolve(strict=True)
    data = path.read_bytes()  # Missing requested artifacts always fail.
    identity = {'path': str(path), 'sha256': hashlib.sha256(data).hexdigest()}
    print(f"{path}: SHA-256 {identity['sha256']}", flush=True)
    require(len(data) >= 52 and data[:7] == b'\x7fELF\x01\x01\x01', 'expected ELF32 little-endian artifact')
    require(struct.unpack_from('<HH', data, 16) == (2, 40), 'expected ARM executable ELF')
    commands = {}
    for tool in ('nm', 'readelf'):
        commands[tool] = shutil.which('arm-none-eabi-' + tool)
        require(commands[tool], f'missing arm-none-eabi-{tool}')
    def run(tool, *args):
        return subprocess.check_output([commands[tool], *args, str(path)], text=True)
    symbols = {}
    for line in run('nm', '-S', '--defined-only').splitlines():
        fields = line.split()
        if len(fields) == 4 and fields[3] in ('_usbd_driver', 'mscd_open', '__wrap_mscd_reset', '__wrap_mscd_control_xfer_cb'):
            require(fields[3] not in symbols, f'duplicate symbol {fields[3]}')
            symbols[fields[3]] = (int(fields[0], 16), int(fields[1], 16))
    for name in ('_usbd_driver', 'mscd_open', '__wrap_mscd_reset', '__wrap_mscd_control_xfer_cb'):
        require(name in symbols, f'missing symbol {name}')
    layout = driver_layout(run('readelf', '--debug-dump=info'))
    address, length = symbols['_usbd_driver']
    section_offset = struct.unpack_from('<I', data, 32)[0]
    entry_size, count = struct.unpack_from('<HH', data, 46)
    require(entry_size == 40 and section_offset + count * entry_size <= len(data), 'malformed ELF section headers')
    tables = []
    for index in range(count):
        _, kind, flags, base, offset, size, *_ = struct.unpack_from('<10I', data, section_offset + index * entry_size)
        if kind != 8 and flags & 2 and base <= address and address + length <= base + size:
            start = offset + address - base
            require(start + length <= len(data), 'truncated driver table')
            tables.append(data[start:start + length])
    require(len(tables) == 1, 'driver table not uniquely mapped to ELF file bytes')
    table = tables[0]
    check_table(table, layout, symbols)
    print(f'{path}: actual MSC reset/control pointers resolve to wrappers')
    return identity


def check_artifacts(paths):
    """Check every requested file; identities do not establish transport provenance."""
    seen = set()
    identities = []
    errors = []
    for requested in paths:
        try:
            path = Path(requested).resolve(strict=True)
            stat = path.stat()
            key = (stat.st_dev, stat.st_ino)
            require(key not in seen, f'duplicate canonical artifact: {path}')
            seen.add(key)
            identities.append(check_elf(path))
        except (OSError, ValueError, KeyError, TypeError, AttributeError,
                struct.error, subprocess.SubprocessError) as error:
            errors.append(f'{Path(requested).absolute()}: {error}')
    if errors:
        raise ValueError('\n'.join(errors))
    return identities


def test_msc_callback_pointers(msc_elf_paths):
    if not msc_elf_paths:
        pytest.fail("MSC artifact tests require --elf PATH (build application firmware first)")
    check_artifacts(msc_elf_paths)


def test_sd_storage_build_dependencies(msc_elf_paths):
    if not msc_elf_paths:
        pytest.fail("Storage dependency checks require --elf PATH and its CMake build manifest")
    for path in msc_elf_paths:
        directory = path.resolve().parent
        commands = json.loads((directory / 'compile_commands.json').read_text())
        sources = [Path(command['file']).as_posix() for command in commands]
        sd_sources = [source for source in sources if '/src/drivers/sdcard/' in source]
        adapter = any(source.endswith('/platform/storage/storage.c') for source in sources)
        assert sd_sources and adapter, f"SD build is missing its protocol or adapter: {directory}"
