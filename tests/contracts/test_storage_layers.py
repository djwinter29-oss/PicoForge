"""Keep physical storage engines independent of device and filesystem policy."""

from pathlib import Path
import re

import pytest

pytestmark = pytest.mark.contracts
ROOT = Path(__file__).resolve().parents[2]
STORAGE = ROOT / "firmware/src/drivers/storage"


def test_physical_engines_do_not_include_device_or_filesystem_headers():
    sources = sorted(STORAGE.rglob("*.c")) + sorted(STORAGE.rglob("*.h"))
    assert sources
    forbidden = re.compile(
        r"(?:sd_card|sdio_pio_protocol|sdio_pio_host|nor_flash|nor_spi|nor_qspi|fatfs|ff\.h|levelx)"
        r"|(?:^|/)(?:sd|nor)(?:_[^/]*)?\.h$"
    )
    for source in sources:
        for header in re.findall(r'^\s*#include\s*[<"]([^>"]+)[>"]', source.read_text(), re.MULTILINE):
            assert header in ("sd_wire.h", "../sd_wire.h") or not forbidden.search(header), (
                f"{source.relative_to(ROOT)} includes device policy: {header}"
            )


@pytest.mark.parametrize("mode", ["spi", "sdio_pio", "qspi_pio"])
def test_each_physical_mode_has_a_concrete_driver(mode):
    for extension in ("c", "h"):
        assert (STORAGE / mode / f"{mode}.{extension}").is_file()
    if mode != "spi":
        assert (STORAGE / mode / f"{mode}.pio").is_file()


@pytest.mark.parametrize("device", ["sd_card", "nor_flash"])
def test_device_drivers_use_only_the_common_physical_transport_boundary(device):
    directory = ROOT / "firmware/src/drivers" / device
    for source in sorted(directory.rglob("*.c")) + sorted(directory.rglob("*.h")):
        text = source.read_text()
        for header in re.findall(r'^\s*#include\s*[<"]([^>"]+)[>"]', text, re.MULTILINE):
            assert not header.startswith(("hardware/", "pico/")), f"{source}: hardware dependency {header}"
            if "storage/" in header:
                assert header.endswith(("/bus.h", "/serial.h", "/sd_wire.h")), (
                    f"{source}: concrete backend include {header}"
                )
        assert not re.search(r"\bpf_(?:spi|sdio_pio|qspi_pio)_\w+", text), source
        assert not re.search(r"\bpf_storage_(?:spi|sdio_pio|qspi_pio)_\w+", text), source
        assert not re.search(r"\b(?:spi_inst_t|dma_channel_\w+|pio_sm_\w+)\b", text), source


def test_common_lifecycle_and_raw_wire_interfaces_do_not_expose_device_transactions():
    common = (STORAGE / "bus.h").read_text()
    assert not re.search(r"\b(?:response_bits|response_words|dummy_cycles|address_bytes|tx_prepare)\b", common)
    assert "pf_storage_sd_wire_state_t" not in common
    serial = (STORAGE / "serial.h").read_text()
    assert not re.search(r"\b(?:command|address_bytes|dummy_cycles)\s*;", serial)
    native = (STORAGE / "sd_wire.h").read_text()
    assert not re.search(r"\b(?:clock_owner|last_response_words|rx_ready|mode_restore_failed)\b", native)
    physical = (STORAGE / "sdio_pio/sdio_pio.c").read_text()
    assert "prepare_80_clocks" not in physical


@pytest.mark.parametrize("mode", ["spi", "sdio_pio", "qspi_pio"])
def test_backend_api_and_guards_match_module_names(mode):
    header = (STORAGE / mode / f"{mode}.h").read_text()
    guard = f"PF_{mode.upper()}_H"
    assert re.search(rf"^#ifndef {guard}\n#define {guard}$", header, re.MULTILINE)
    assert f"pf_{mode}_t" in header
    assert f"pf_{mode}_bus(" in header
    for source in (STORAGE / mode).rglob("*"):
        if source.suffix in (".c", ".h", ".pio"):
            assert not re.search(rf"\bpf_storage_{mode}_(?!pins_)\w+", source.read_text()), source
            assert not re.search(rf"\bPF_STORAGE_{mode.upper()}_H\b", source.read_text()), source


@pytest.mark.parametrize("module", ["bus", "serial", "sd_wire"])
def test_wire_interface_names_and_guards_match_modules(module):
    header = (STORAGE / f"{module}.h").read_text()
    guard = f"PF_STORAGE_{module.upper()}_H"
    assert re.search(rf"^#ifndef {guard}\n#define {guard}$", header, re.MULTILINE)
    assert f"pf_storage_{module}_" in header
    assert (STORAGE / f"{module}.c").is_file()
    for extension in ("c", "h"):
        text = (STORAGE / f"{module}.{extension}").read_text()
        assert not re.search(r"\b(?:pf_storage_transport\w*|pf_storage_native\w*|PF_STORAGE_TRANSPORT\w*"
                             r"|PF_STORAGE_NATIVE\w*)\b", text)
