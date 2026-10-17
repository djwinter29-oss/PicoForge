from pathlib import Path

import pytest


def pytest_addoption(parser):
    parser.addoption("--elf", action="append", default=[], help="MSC application ELF; repeat for multiple artifacts")
    parser.addoption("--baremetal-build-dir", action="append", default=[],
                     help="Bare-metal build directory; repeat for multiple builds")


def pytest_generate_tests(metafunc):
    for fixture, option in (("msc_elf_paths", "--elf"), ("baremetal_build_dir", "--baremetal-build-dir")):
        if fixture not in metafunc.fixturenames:
            continue
        paths = [Path(value) for value in metafunc.config.getoption(option)]
        # Check MSC inputs together so duplicate artifact identities remain an error.
        values = [paths] if fixture == "msc_elf_paths" else paths
        metafunc.parametrize(fixture, values or [None])
