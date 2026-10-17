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
    forbidden = re.compile(r"(?:sd_card|sdio_pio_protocol|sdio_pio_host|nor_flash|nor_spi|nor_qspi|fatfs|ff\.h|levelx)")
    for source in sources:
        for header in re.findall(r'^\s*#include\s*[<"]([^>"]+)[>"]', source.read_text(), re.MULTILINE):
            assert not forbidden.search(header), f"{source.relative_to(ROOT)} includes device policy: {header}"


@pytest.mark.parametrize("mode", ["spi", "sdio_pio", "qspi_pio"])
def test_each_physical_mode_has_a_concrete_driver(mode):
    for extension in ("c", "h"):
        assert (STORAGE / mode / f"storage_{mode}.{extension}").is_file()
    if mode != "spi":
        assert (STORAGE / mode / f"storage_{mode}.pio").is_file()
