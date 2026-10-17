#!/usr/bin/env python3
"""Validation regressions using copied sources and real ARM ELF binutils."""
import hashlib
import os
import runpy
import shlex
from pathlib import Path
import shutil
import struct
import pytest

ROOT = Path(__file__).resolve().parents[1]
ARTIFACT_TESTS = ROOT / 'tests/artifacts'
pytestmark = pytest.mark.contracts


def test_ceedling_tests_mirror_source_directories_and_module_names():
    source_root = ROOT / 'firmware/src'
    test_root = ROOT / 'firmware/tests'
    tests = sorted(test_root.rglob('test_*.c'))
    assert tests
    for test in tests:
        relative = test.relative_to(test_root)
        source_directory = source_root / relative.parent
        candidates = [
            source for source in source_directory.glob('*.c')
            if test.stem == f'test_{source.stem}' or test.stem.startswith(f'test_{source.stem}_')
        ]
        assert candidates, (
            f'{relative}: expected test_<source_basename>[_<scenario>].c alongside mirrored source directory'
        )


def test_ceedling_ci_initializes_both_source_dependencies_before_setup():
    workflow = (ROOT / '.github/workflows/host-validation.yml').read_text()
    setup = workflow.split('  ceedling-setup:', 1)[1]
    initialization, _ = setup.split('tools/validation/setup-ceedling-testing.sh', 1)
    commands = [
        shlex.split(line.strip().removeprefix('run: '))
        for line in initialization.splitlines() if line.strip().startswith('run: git submodule ')
    ]
    assert len(commands) == 1
    assert commands[0][:4] == ['git', 'submodule', 'update', '--init']
    assert set(commands[0][4:]) == {
        'firmware/third_party/embedded-cli', 'firmware/third_party/levelx',
    }


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


@pytest.fixture
def checker(monkeypatch):
    monkeypatch.setenv("CC", "/nonexistent/compiler")
    return runpy.run_path(str(ARTIFACT_TESTS / 'test_msc_elf.py'))


def test_source_contract_rejects_changed_ordering(tmp_path, monkeypatch):
    monkeypatch.setenv("CC", "/nonexistent/compiler")
    contract = runpy.run_path(str(ROOT / 'tests/contracts/test_tinyusb_msc_contract.py'))
    for relative in ('firmware/CMakeLists.txt',
                     'firmware/third_party/pico-sdk/lib/tinyusb/src/class/msc/msc_device.c',
                     'firmware/third_party/pico-sdk/lib/tinyusb/src/device/usbd.c'):
        dest = tmp_path / relative
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / relative, dest)
    contract['check_contract'](tmp_path)
    source = tmp_path / 'firmware/third_party/pico-sdk/lib/tinyusb/src/class/msc/msc_device.c'
    source.write_text(source.read_text().replace(
        'set_sense_medium_not_present(p_cbw->lun);', '/* missing failure sense */'))
    with pytest.raises(ValueError):
        contract['check_contract'](tmp_path)


def test_elf_identity(checker, tmp_path, capsys):
    paths = [tmp_path / name for name in ('first.elf', 'second.elf')]
    for path in paths:
        elf_fixture(path)
    identities = checker['check_artifacts'](paths)
    output = capsys.readouterr().out
    for path, identity in zip(paths, identities):
        assert identity == {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
        assert f"{path}: SHA-256 {identity['sha256']}" in output
        assert f'{path}: actual MSC' in output


@pytest.mark.parametrize("alias_kind", ("same", "symlink", "hardlink"))
def test_duplicate_elf_rejected(checker, tmp_path, capsys, alias_kind):
    first, second, alias = (tmp_path / name for name in ('first.elf', 'second.elf', 'alias.elf'))
    elf_fixture(first)
    elf_fixture(second)
    if alias_kind == "symlink":
        alias.symlink_to(first)
    elif alias_kind == "hardlink":
        os.link(first, alias)
    else:
        alias = first
    with pytest.raises(ValueError, match="duplicate canonical artifact"):
        checker['check_artifacts']([first, alias, second])
    assert f'{second}: actual MSC' in capsys.readouterr().out


@pytest.mark.parametrize("fault,message", (
    ('reset', '__wrap_mscd_reset'), ('control', '__wrap_mscd_control_xfer_cb'),
    ('layout', 'overlapping members')))
def test_broken_elf_rejected(checker, tmp_path, capsys, fault, message):
    bad, good = tmp_path / 'bad.elf', tmp_path / 'good.elf'
    elf_fixture(bad, fault)
    elf_fixture(good)
    with pytest.raises(ValueError, match=message) as error:
        checker['check_artifacts']([bad, good])
    assert str(bad) in str(error.value)
    assert f'{good}: actual MSC' in capsys.readouterr().out


def test_invalid_and_missing_elfs_reported(checker, tmp_path, capsys):
    invalid, missing, good = (tmp_path / name for name in ('invalid.elf', 'missing.elf', 'good.elf'))
    invalid.write_bytes(b'not ELF')
    elf_fixture(good)
    with pytest.raises(ValueError, match='expected ELF32') as error:
        checker['check_artifacts']([invalid, missing, good])
    assert str(missing) in str(error.value)
    output = capsys.readouterr().out
    assert hashlib.sha256(invalid.read_bytes()).hexdigest() in output
    assert f'{good}: actual MSC' in output


def test_malformed_dwarf_rejected(checker):
    with pytest.raises(ValueError, match='missing or conflicting DWARF'):
        checker['driver_layout']('malformed DWARF')
