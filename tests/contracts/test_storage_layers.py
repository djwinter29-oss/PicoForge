"""Keep physical storage engines independent of device and filesystem policy."""

from pathlib import Path
import re

import pytest

pytestmark = pytest.mark.contracts
ROOT = Path(__file__).resolve().parents[2]
SD_CARD = ROOT / "firmware/src/drivers/sd_card"
LINK = SD_CARD / "link"
PHYSICAL = (LINK, SD_CARD / "sdio_pio", SD_CARD / "spi")


def test_project_c_includes_do_not_traverse_relative_directories():
    for directory in ("firmware/src", "firmware/tests", "host", "tools"):
        for source in (ROOT / directory).rglob("*"):
            if source.suffix not in (".c", ".h", ".inc") or "third_party" in source.parts:
                continue
            assert not re.search(
                r'^\s*#\s*(?:include\s*|define\s+\w+\s+)[<"]\.\.?/',
                source.read_text(), re.MULTILINE
            ), source


def test_storage_application_policy_stays_out_of_middleware():
    middleware = ROOT / "firmware/src/middleware/storage"
    application = ROOT / "firmware/src/app/storage"
    for module in ("storage_commands", "file_benchmark", "storage_worker", "fatfs_volume"):
        for extension in ("c", "h"):
            assert (application / f"{module}.{extension}").is_file()
            assert not (middleware / f"{module}.{extension}").exists()
    for source in sorted(middleware.glob("*.c")) + sorted(middleware.glob("*.h")):
        headers = re.findall(r'^\s*#include\s*[<"]([^>"]+)[>"]', source.read_text(), re.MULTILINE)
        assert not any(header.startswith("app/") or Path(header).name in (
            "storage_commands.h", "file_benchmark.h", "storage_worker.h", "fatfs_volume.h", "cli.h"
        ) for header in headers), source


def test_fatfs_volume_does_not_depend_on_the_sd_backend():
    application = ROOT / "firmware/src/app/storage"
    for extension in ("c", "h"):
        source = application / f"fatfs_volume.{extension}"
        assert not re.search(r"\b(?:pf_sd_\w+|PF_SD_\w+)\b", source.read_text()), source
        headers = re.findall(r'^\s*#include\s*[<"]([^>"]+)[>"]', source.read_text(), re.MULTILINE)
        assert not any("sd_card/" in header or Path(header).name in (
            "sd.h", "sd_block.h", "sd_block_device.h"
        ) for header in headers), source


def test_product_cli_is_application_owned_and_logging_does_not_select_uart():
    for extension in ("c", "h"):
        assert (ROOT / f"firmware/src/app/cli/cli.{extension}").is_file()
        assert not (ROOT / f"firmware/src/middleware/cli/cli.{extension}").exists()
    for source in (ROOT / "firmware/src/middleware").rglob("*.[ch]"):
        text = source.read_text()
        headers = re.findall(r'^\s*#include\s*[<"]([^>"]+)[>"]', text, re.MULTILINE)
        assert not any(header.startswith("app/") or Path(header).name in (
            "cli.h", "fatfs_volume.h"
        ) for header in headers), source
    logging = (ROOT / "firmware/src/middleware/logging/logging.c").read_text()
    assert "stdio_filter_driver" not in logging
    assert "stdio_uart" not in logging
    monitor = (ROOT / "firmware/src/drivers/monitor_uart.c").read_text()
    assert monitor.index("stdio_uart_init_full(") < monitor.index("stdio_filter_driver(&stdio_uart)")
    assert "pf_monitor_uart_init(" in (ROOT / "firmware/src/main.c").read_text()


def test_file_benchmark_and_application_headers_do_not_expose_sd():
    application = ROOT / "firmware/src/app/storage"
    sources = [application / "file_benchmark.c", *(
        source for source in application.glob("*.h") if source.name != "sd_media.h"
    )]
    for source in sources:
        text = source.read_text()
        assert not re.search(r"\b(?:pf_sd_\w+|PF_SD_\w+)\b", text), source
        assert not re.search(r'#include\s*"[^"]*(?:sd_card/|sd_block\.h|sd\.h)', text), source


def test_usb_product_behavior_lives_in_the_application():
    application = ROOT / "firmware/src/app/usb"
    for module in ("usb_task", "usb_hid", "usb_msc"):
        for extension in ("c", "h"):
            assert (application / f"{module}.{extension}").is_file()
    assert (application / "usb_descriptors.c").is_file()
    assert (ROOT / "firmware/src/integration/usb/tusb_config.h").is_file()
    assert not list((ROOT / "firmware/src/usb").glob("*.[ch]"))


def test_sd_ownership_policy_stays_above_driver_and_middleware():
    driver = ROOT / "firmware/src/drivers/sd_card"
    for source in (driver / "sd.c", driver / "sd.h", driver / "sd_block.h"):
        text = source.read_text()
        assert not re.search(r"\b(?:active_owner|reinit_after_usb_owner|pf_sd_media_\w+|PF_SD_OWNER_\w+)\b", text), source
        assert not re.search(r'#include\s*"[^"]*(?:app/|middleware/)', text), source
    assert not (driver / "sd_msc.h").exists()
    for source in (ROOT / "firmware/src/app").rglob("*.[ch]"):
        if source.name == "sd_media.c":
            continue
        assert not re.search(
            r"\bpf_sd_(?:init|read_blocks|write_blocks|sync|block_device)\s*\(", source.read_text()
        ), source
    for source in (ROOT / "firmware/src/middleware/storage").glob("*.[ch]"):
        assert not re.search(r'#include\s*"[^"]*app/', source.read_text()), source


def test_physical_engines_do_not_include_device_or_filesystem_headers():
    sources = [source for directory in PHYSICAL for source in sorted(directory.rglob("*.[ch]"))]
    assert sources
    physical = re.compile(r"^(?:drivers/sd_card/(?:link|sdio_pio|spi)/)?(?:sd_link\w*|sdio_pio\w*|spi\w*)\.h$")
    forbidden = re.compile(
        r"(?:sd_card|sdio_pio_protocol|sdio_pio_host|fatfs|ff\.h)"
        r"|(?:^|/)sd(?:_[^/]*)?\.h$"
    )
    for source in sources:
        for header in re.findall(r'^\s*#include\s*[<"]([^>"]+)[>"]', source.read_text(), re.MULTILINE):
            assert physical.match(header) or not forbidden.search(header), (
                f"{source.relative_to(ROOT)} includes device policy: {header}"
            )


@pytest.mark.parametrize("mode", ["spi", "sdio_pio"])
def test_each_physical_mode_has_a_concrete_driver(mode):
    for extension in ("c", "h"):
        assert (SD_CARD / mode / f"{mode}.{extension}").is_file()
    if mode != "spi":
        assert (SD_CARD / mode / f"{mode}.pio").is_file()


@pytest.mark.parametrize("device", ["sd_card"])
def test_device_drivers_use_only_the_common_physical_transport_boundary(device):
    directory = ROOT / "firmware/src/drivers" / device
    for source in sorted(directory.glob("*.c")) + sorted(directory.glob("*.h")):
        # sd_board.c is the single board-wiring seam that binds the build-selected backend and card detect.
        if source.name == "sd_board.c":
            continue
        text = source.read_text()
        for header in re.findall(r'^\s*#include\s*[<"]([^>"]+)[>"]', text, re.MULTILINE):
            assert not header.startswith(("hardware/", "pico/")), f"{source}: hardware dependency {header}"
            if "sd_card/" in header:
                assert header.startswith("drivers/sd_card/link/"), (
                    f"{source}: concrete backend include {header}"
                )
        assert not re.search(r"\bpf_(?:spi|sdio_pio)_\w+", text), source
        assert not re.search(r"\bpf_storage_(?:spi|sdio_pio)_\w+", text), source
        assert not re.search(r"\b(?:spi_inst_t|dma_channel_\w+|pio_sm_\w+)\b", text), source


def test_common_lifecycle_and_raw_wire_interfaces_do_not_expose_device_transactions():
    common = (LINK / "sd_link.h").read_text()
    assert not re.search(r"\b(?:response_bits|response_words|dummy_cycles|address_bytes|tx_prepare)\b", common)
    assert "pf_sd_link_wire_state_t" not in common
    serial = (LINK / "sd_link_serial.h").read_text()
    assert not re.search(r"\b(?:command|address_bytes|dummy_cycles)\s*;", serial)
    native = (LINK / "sd_link_wire.h").read_text()
    assert not re.search(r"\b(?:clock_owner|last_response_words|rx_ready|mode_restore_failed)\b", native)
    physical = (SD_CARD / "sdio_pio/sdio_pio.c").read_text()
    assert "prepare_80_clocks" not in physical


@pytest.mark.parametrize("mode", ["spi", "sdio_pio"])
def test_backend_api_and_guards_match_module_names(mode):
    header = (SD_CARD / mode / f"{mode}.h").read_text()
    guard = f"PF_{mode.upper()}_H"
    assert re.search(rf"^#ifndef {guard}\n#define {guard}$", header, re.MULTILINE)
    assert f"pf_{mode}_t" in header
    assert f"pf_{mode}_bus(" in header
    for source in (SD_CARD / mode).rglob("*"):
        if source.suffix in (".c", ".h", ".pio"):
            assert not re.search(rf"\bpf_storage_{mode}_(?!pins_)\w+", source.read_text()), source
            assert not re.search(rf"\bPF_STORAGE_{mode.upper()}_H\b", source.read_text()), source


@pytest.mark.parametrize("module", ["sd_link", "sd_link_serial", "sd_link_wire"])
def test_wire_interface_names_and_guards_match_modules(module):
    header = (LINK / f"{module}.h").read_text()
    guard = f"PF_{module.upper()}_H"
    assert re.search(rf"^#ifndef {guard}\n#define {guard}$", header, re.MULTILINE)
    assert f"pf_{module}_" in header
    assert (LINK / f"{module}.c").is_file()
    for extension in ("c", "h"):
        text = (LINK / f"{module}.{extension}").read_text()
        assert not re.search(r"\b(?:pf_storage_transport\w*|pf_storage_native\w*|PF_STORAGE_TRANSPORT\w*"
                             r"|PF_STORAGE_NATIVE\w*)\b", text)
