#!/usr/bin/env python3
"""Focused subprocess regressions using copied sources and real ELF binutils."""
import hashlib
import os
import runpy
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
VALIDATION = ROOT / 'tools/validation'


def elf_fixture(path, broken=None):
    """Encode a minimal ARM ELF/DWARF fixture; no compiler or firmware build."""
    # DWARF v4 abbreviations: CU, structure, member, pointer, typedef.
    abbrev = bytes([
        1, 0x11, 1, 0, 0,
        2, 0x13, 1, 3, 8, 0x0b, 0x0b, 0, 0,
        3, 0x0d, 0, 3, 8, 0x49, 0x13, 0x38, 0x0b, 0, 0,
        4, 0x0f, 0, 0x0b, 0x0b, 0, 0,
        5, 0x16, 0, 3, 8, 0x49, 0x13, 0, 0, 0])
    body = bytearray(b'\x01')
    struct_ref = 11 + len(body)
    body += b'\x02driver\0\x0c'
    refs = []
    for name, offset in (('open', 0), ('reset', 4), ('control_xfer_cb', 8)):
        body += b'\x03' + name.encode() + b'\0'
        refs.append(len(body))
        body += b'\0' * 4 + bytes([0 if broken == 'layout' else offset])
    body += b'\0'
    pointer_ref = 11 + len(body)
    body += b'\x04\x04\x05usbd_class_driver_t\0' + struct.pack('<I', struct_ref) + b'\0'
    for ref in refs:
        struct.pack_into('<I', body, ref, pointer_ref)
    debug = struct.pack('<IHI B', len(body) + 7, 4, 0, 4) + body
    strings = bytearray(b'\0')
    symbols = bytearray(16)
    for name, address, size, info in (
        ('_usbd_driver', 0x2000, 12, 0x11), ('mscd_open', 0x2100, 2, 0x12),
        ('__wrap_mscd_reset', 0x2200, 2, 0x12),
        ('__wrap_mscd_control_xfer_cb', 0x2300, 2, 0x12)):
        index = len(strings)
        strings += name.encode() + b'\0'
        symbols += struct.pack('<IIIBBH', index, address, size, info, 0, 1)
    table = struct.pack('<III', 0x2101, 0 if broken == 'reset' else 0x2201,
                        0 if broken == 'control' else 0x2301)
    sections = [('', b'', 0, 0, 0, 0, 0, 0),
                ('.data', table, 1, 3, 0x2000, 0, 0, 0),
                ('.symtab', symbols, 2, 0, 0, 3, 1, 16),
                ('.strtab', strings, 3, 0, 0, 0, 0, 0),
                ('.debug_abbrev', abbrev, 1, 0, 0, 0, 0, 0),
                ('.debug_info', debug, 1, 0, 0, 0, 0, 0)]
    names = b'\0' + b''.join(s[0].encode() + b'\0' for s in sections[1:]) + b'.shstrtab\0'
    sections.append(('.shstrtab', names, 3, 0, 0, 0, 0, 0))
    data = bytearray(52)
    headers = []
    for name, content, kind, flags, base, link, info, entsize in sections:
        data += b'\0' * (-len(data) % 4)
        offset = len(data)
        data += content
        headers.append(struct.pack('<10I', names.index(name.encode() + b'\0') if name else 0,
                                   kind, flags, base, offset, len(content), link, info, 4, entsize))
    section_offset = len(data)
    data += b''.join(headers)
    data[:52] = struct.pack('<16sHHIIIIIHHHHHH', b'\x7fELF\x01\x01\x01' + b'\0'*9,
                            2, 40, 1, 0, 0, section_offset, 0, 52, 0, 0, 40, len(sections), 6)
    path.write_bytes(data)


class Integrity(unittest.TestCase):
    def run_script(self, script, *args, env=None):
        return subprocess.run([sys.executable, str(script), *map(str, args)],
                              cwd=ROOT, env=env, capture_output=True, text=True, timeout=60)

    def test_source_order_checker_needs_no_compiler_and_rejects_changed_ordering(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            files = [VALIDATION / 'check-tinyusb-msc-contract.py'] + [ROOT / p for p in (
                'firmware/CMakeLists.txt',
                'firmware/third_party/pico-sdk/lib/tinyusb/src/class/msc/msc_device.c',
                'firmware/third_party/pico-sdk/lib/tinyusb/src/device/usbd.c')]
            for source in files:
                dest = root / source.relative_to(ROOT)
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, dest)
            runner = root / 'tools/validation/check-tinyusb-msc-contract.py'
            env = dict(os.environ, CC='/nonexistent/compiler', PYTHONDONTWRITEBYTECODE='1')
            result = self.run_script(runner, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('Pinned TinyUSB source ordering checked', result.stdout)
            source = root / 'firmware/third_party/pico-sdk/lib/tinyusb/src/class/msc/msc_device.c'
            source.write_text(source.read_text().replace(
                'set_sense_medium_not_present(p_cbw->lun);', '/* missing failure sense */'))
            result = self.run_script(runner, env=env)
            self.assertNotEqual(result.returncode, 0)

    def test_elf_standalone_and_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            first, second = root / 'first.elf', root / 'second.elf'
            elf_fixture(first)
            elf_fixture(second)
            env = dict(os.environ, CC='/nonexistent/compiler', PYTHONDONTWRITEBYTECODE='1')
            checker = root / 'check-msc-elf.py'
            shutil.copyfile(VALIDATION / checker.name, checker)
            result = self.run_script(checker, first, second, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for path in (first, second):
                self.assertIn(f'{path}: SHA-256 {hashlib.sha256(path.read_bytes()).hexdigest()}', result.stdout)
                self.assertIn(f'{path}: actual MSC', result.stdout)
            self.assertNotIn('Executed required regression', result.stdout)
            alias = root / 'alias.elf'
            alias.symlink_to(first)
            hardlink = root / 'hardlink.elf'
            os.link(first, hardlink)
            for duplicate in (first, alias, hardlink):
                result = self.run_script(checker, first, duplicate, second, env=env)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('duplicate canonical artifact', result.stderr)
                self.assertIn(f'{second}: actual MSC', result.stdout)
            for fault, message in (('reset', '__wrap_mscd_reset'),
                                   ('control', '__wrap_mscd_control_xfer_cb'),
                                   ('layout', 'overlapping members')):
                bad = root / f'{fault}.elf'
                elf_fixture(bad, fault)
                result = self.run_script(checker, bad, second, env=env)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
                self.assertIn(str(bad), result.stderr)
                self.assertIn(f'{second}: actual MSC', result.stdout)
            invalid = root / 'invalid.elf'
            invalid.write_bytes(b'not ELF')
            result = self.run_script(checker, invalid, root / 'missing.elf', second, env=env)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('expected ELF32', result.stderr)
            self.assertIn('missing.elf', result.stderr)
            self.assertIn(hashlib.sha256(invalid.read_bytes()).hexdigest(), result.stdout)
            self.assertIn(f'{second}: actual MSC', result.stdout)
            result = self.run_script(checker, env=env)
            self.assertNotEqual(result.returncode, 0)

    def test_elf_checker_rejects_malformed_dwarf_and_each_wrong_callback(self):
        checker = runpy.run_path(str(VALIDATION / 'check-msc-elf.py'))
        with self.assertRaisesRegex(ValueError, 'missing or conflicting DWARF'):
            checker['driver_layout']('malformed DWARF')
        layout = (12, 0, 4, 8)
        symbols = {'mscd_open': (0x2100, 2), '__wrap_mscd_reset': (0x2200, 2),
                   '__wrap_mscd_control_xfer_cb': (0x2300, 2)}
        table = struct.pack('<III', 0x2101, 0x2201, 0x2301)
        self.assertEqual(checker['check_table'](table, layout, symbols), 0)
        for offset, name in ((4, '__wrap_mscd_reset'), (8, '__wrap_mscd_control_xfer_cb')):
            broken = bytearray(table)
            struct.pack_into('<I', broken, offset, 0)
            with self.subTest(callback=name), self.assertRaisesRegex(ValueError, name):
                checker['check_table'](broken, layout, symbols)


if __name__ == '__main__':
    unittest.main(verbosity=2)
