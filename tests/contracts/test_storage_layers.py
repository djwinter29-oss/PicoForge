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
            assert not forbidden.search(header), f"{source.relative_to(ROOT)} includes device policy: {header}"


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
                assert header.endswith("/storage_transport.h"), f"{source}: concrete backend include {header}"
        assert not re.search(r"\bpf_(?:spi|sdio_pio|qspi_pio)_\w+", text), source
        assert not re.search(r"\bpf_storage_(?:spi|sdio_pio|qspi_pio)_\w+", text), source
        assert not re.search(r"\b(?:spi_inst_t|dma_channel_\w+|pio_sm_\w+)\b", text), source


@pytest.mark.parametrize("mode", ["spi", "sdio_pio", "qspi_pio"])
def test_backend_api_and_guards_match_module_names(mode):
    header = (STORAGE / mode / f"{mode}.h").read_text()
    guard = f"PF_{mode.upper()}_H"
    assert re.search(rf"^#ifndef {guard}\n#define {guard}$", header, re.MULTILINE)
    assert f"pf_{mode}_t" in header
    assert f"pf_{mode}_transport(" in header
    for source in (STORAGE / mode).rglob("*"):
        if source.suffix in (".c", ".h", ".pio"):
            assert not re.search(rf"\bpf_storage_{mode}_(?!pins_)\w+", source.read_text()), source
            assert not re.search(rf"\bPF_STORAGE_{mode.upper()}_H\b", source.read_text()), source
