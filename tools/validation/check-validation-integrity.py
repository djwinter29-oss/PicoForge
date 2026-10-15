#!/usr/bin/env python3
"""Focused subprocess regressions using copied sources and real ELF binutils."""
import hashlib
import os
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

    def test_required_sources_real_runner(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            # Copy only the runner's explicit source inputs, never alter the worktree.
            files = [VALIDATION / 'check-sd-msc-production.py']
            files += list((VALIDATION / 'host').rglob('*.c'))
            files += list((VALIDATION / 'host/stubs').rglob('*.h'))
            files += list((ROOT / 'firmware/src/drivers/sd_card').rglob('*.c'))
            files += list((ROOT / 'firmware/src/drivers/sd_card').rglob('*.h'))
            files += [ROOT / p for p in (
                'firmware/src/config/config.h', 'firmware/src/usb/msc.c',
                'firmware/src/usb/msc.h', 'firmware/src/middleware/storage/storage.h',
                'firmware/src/middleware/storage/storage_worker.h', 'firmware/CMakeLists.txt',
                'firmware/third_party/pico-sdk/lib/tinyusb/src/class/msc/msc_device.c',
                'firmware/third_party/pico-sdk/lib/tinyusb/src/device/usbd.c')]
            for source in files:
                dest = root / source.relative_to(ROOT)
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, dest)
            runner = root / 'tools/validation/check-sd-msc-production.py'
            for name in ('spi_write', 'msc'):
                source = root / f'tools/validation/host/regression_{name}.c'
                saved = source.read_bytes()
                source.unlink()  # Only the temporary copy.
                result = self.run_script(runner)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('Missing required production regression', result.stderr)
                self.assertIn(str(source), result.stderr)
                self.assertNotIn('Executed required regression', result.stdout)
                source.write_bytes(saved)
            result = self.run_script(runner)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for name in ('spi_write', 'msc'):
                self.assertIn(f'Executed required regression: {root}/tools/validation/host/regression_{name}.c', result.stdout)

    def test_elf_standalone_and_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            first, second = root / 'first.elf', root / 'second.elf'
            elf_fixture(first)
            elf_fixture(second)
            env = dict(os.environ, CC='/nonexistent/compiler', PYTHONDONTWRITEBYTECODE='1')
            checker = VALIDATION / 'check-msc-elf.py'
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


if __name__ == '__main__':
    unittest.main(verbosity=2)
