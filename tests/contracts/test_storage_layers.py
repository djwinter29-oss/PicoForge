"""Keep physical storage engines independent of device and filesystem policy."""

from pathlib import Path
import re

import pytest

pytestmark = pytest.mark.contracts
ROOT = Path(__file__).resolve().parents[2]
SDCARD = ROOT / "firmware/src/drivers/sdcard"
ENGINES = {"sdio_pio": SDCARD / "sdio", "spi": SDCARD / "spi"}
PHYSICAL_SOURCES = [
    *SDCARD.glob("sd_link*.[ch]"),
    SDCARD / "sdio/sd_link_sdio.h",
    SDCARD / "spi/sd_link_spi.h",
    *ENGINES["sdio_pio"].glob("sdio_*.[ch]"),
    *ENGINES["spi"].glob("spi*.[ch]"),
]


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
    for module in ("file_benchmark", "storage_worker"):
        for extension in ("c", "h"):
            assert (application / f"{module}.{extension}").is_file()
            assert not (middleware / f"{module}.{extension}").exists()
    for extension in ("c", "h"):
        assert (ROOT / f"firmware/src/app/cli/cli_storage.{extension}").is_file()
        assert not (application / f"storage_commands.{extension}").exists()
    for source in sorted(middleware.glob("*.c")) + sorted(middleware.glob("*.h")):
        headers = re.findall(r'^\s*#include\s*[<"]([^>"]+)[>"]', source.read_text(), re.MULTILINE)
        assert not any(header.startswith("app/") or Path(header).name in (
            "cli_storage.h", "file_benchmark.h", "storage_worker.h", "cli.h"
        ) for header in headers), source


def test_fatfs_volume_does_not_depend_on_the_sd_backend():
    middleware = ROOT / "firmware/src/middleware/storage"
    for extension in ("c", "h"):
        source = middleware / f"fatfs_volume.{extension}"
        assert not re.search(r"\b(?:pf_sd_\w+|PF_SD_\w+)\b", source.read_text()), source
        headers = re.findall(r'^\s*#include\s*[<"]([^>"]+)[>"]', source.read_text(), re.MULTILINE)
        assert not any("sdcard/" in header or Path(header).name in (
            "sd.h", "sd_block.h"
        ) for header in headers), source


def test_product_cli_is_application_owned_and_logging_does_not_select_uart():
    for extension in ("c", "h"):
        assert (ROOT / f"firmware/src/app/cli/cli.{extension}").is_file()
        assert not (ROOT / f"firmware/src/middleware/cli/cli.{extension}").exists()
    for source in (ROOT / "firmware/src/middleware").rglob("*.[ch]"):
        text = source.read_text()
        headers = re.findall(r'^\s*#include\s*[<"]([^>"]+)[>"]', text, re.MULTILINE)
        assert not any(header.startswith("app/") or Path(header).name in (
            "cli.h",
        ) for header in headers), source
    logging = (ROOT / "firmware/src/middleware/logging/logging.c").read_text()
    assert "stdio_filter_driver" not in logging
    assert "stdio_uart" not in logging
    monitor = (ROOT / "firmware/src/drivers/monitor_uart.c").read_text()
    assert monitor.index("stdio_uart_init_full(") < monitor.index("stdio_filter_driver(&stdio_uart)")
    assert "pf_monitor_uart_init(" in (ROOT / "firmware/src/main.c").read_text()


def test_file_benchmark_and_application_headers_do_not_expose_sd():
    application = ROOT / "firmware/src/app/storage"
    sources = [application / "file_benchmark.c", *application.glob("*.h")]
    for source in sources:
        text = source.read_text()
        assert not re.search(r"\b(?:pf_sd_\w+|PF_SD_\w+)\b", text), source
        assert not re.search(r'#include\s*"[^"]*(?:sdcard/|sd_block\.h|sd\.h)', text), source


def test_usb_product_behavior_lives_in_the_application():
    application = ROOT / "firmware/src/app/usb"
    for module in ("usb_task", "usb_hid", "usb_msc"):
        for extension in ("c", "h"):
            assert (application / f"{module}.{extension}").is_file()
    assert (application / "usb_descriptors.c").is_file()
    assert (ROOT / "firmware/src/integration/usb/tusb_config.h").is_file()
    assert not list((ROOT / "firmware/src/usb").glob("*.[ch]"))


def test_sd_ownership_policy_stays_above_driver_and_middleware():
    driver = ROOT / "firmware/src/drivers/sdcard"
    for source in (driver / "sd.c", driver / "sd.h"):
        text = source.read_text()
        assert not re.search(r"\b(?:active_owner|reinit_after_usb_owner|pf_storage_\w+|PF_SD_OWNER_\w+)\b", text), source
        assert not re.search(r'#include\s*"[^"]*(?:app/|middleware/)', text), source
    assert not (driver / "sd_msc.h").exists()
    # Above middleware, storage is a filesystem exported through middleware/storage/storage.h.
    for source in (ROOT / "firmware/src/app").rglob("*.[ch]"):
        text = source.read_text()
        assert not re.search(r"\bpf_sd_\w+|\bPF_SD_\w+", text), source
        assert not re.search(r'#include\s*"[^"]*(?:sdcard/|sd_block\.h|sd\.h)', text), source
    for source in (ROOT / "firmware/src/middleware/storage").glob("*.[ch]"):
        assert not re.search(r'#include\s*"[^"]*app/', source.read_text()), source
    # The CLI registers its executor with the worker; the worker never includes the CLI.
    for source in (ROOT / "firmware/src/app/storage").glob("*.[ch]"):
        assert not re.search(r'#include\s*"[^"]*cli', source.read_text()), source
    # CMake selects implementations; no source file selects the transport with a preprocessor branch.
    for source in (ROOT / "firmware/src").rglob("*.[ch]"):
        assert "PF_CONFIG_SDCARD_TRANSPORT" not in source.read_text(), source
    storage_api = (ROOT / "firmware/src/middleware/storage/storage.h").read_text()
    assert not re.search(r'#include\s*"[^"]*drivers/', storage_api)
    assert not re.search(r"\bpf_sd_\w+|\bPF_SD_\w+", storage_api)


def test_physical_engines_do_not_include_device_or_filesystem_headers():
    assert PHYSICAL_SOURCES
    physical = re.compile(r"^(?:drivers/sdcard/(?:sdio/|spi/)?)?(?:sd_link\w*|sdio_pio\w*|spi\w*)\.h$")
    forbidden = re.compile(
        r"(?:sdcard|sdio_pio_protocol|sdio_pio_host|fatfs|ff\.h)"
        r"|(?:^|/)sd(?:_[^/]*)?\.h$"
    )
    for source in PHYSICAL_SOURCES:
        for header in re.findall(r'^\s*#include\s*[<"]([^>"]+)[>"]', source.read_text(), re.MULTILINE):
            assert physical.match(header) or not forbidden.search(header), (
                f"{source.relative_to(ROOT)} includes device policy: {header}"
            )


@pytest.mark.parametrize("mode", ["spi", "sdio_pio"])
def test_each_physical_mode_has_a_concrete_driver(mode):
    for extension in ("c", "h"):
        assert (ENGINES[mode] / f"{mode}.{extension}").is_file()
    if mode != "spi":
        assert (ENGINES[mode] / f"{mode}.pio").is_file()


@pytest.mark.parametrize("device", ["sdcard"])
def test_device_drivers_use_only_the_common_physical_transport_boundary(device):
    directory = ROOT / "firmware/src/drivers" / device
    sources = [
        *directory.glob("*.[ch]"),
        *directory.glob("sdio/sd_*.[ch]"),
        *directory.glob("spi/sd_*.[ch]"),
    ]
    for source in sources:
        # Board composition is the only seam allowed to include concrete hardware engines.
        if source.name in ("sd_board.c", "sd_board_sdio.c", "sd_board_spi.c"):
            continue
        text = source.read_text()
        for header in re.findall(r'^\s*#include\s*[<"]([^>"]+)[>"]', text, re.MULTILINE):
            assert not header.startswith(("hardware/", "pico/")), f"{source}: hardware dependency {header}"
            assert Path(header).name not in (
                "sdio_pio.h", "sdio_pio_pins.h", "spi.h", "spi_pins.h"
            ), f"{source}: concrete backend include {header}"
            if "sdcard/" in header:
                assert re.fullmatch(r"drivers/sdcard/(?:sdio/|spi/)?sd_link\w*\.h", header), (
                    f"{source}: concrete backend include {header}"
                )
        assert not re.search(r"\bpf_(?:spi|sdio_pio)_\w+", text), source
        assert not re.search(r"\bpf_storage_(?:spi|sdio_pio)_\w+", text), source
        assert not re.search(r"\b(?:spi_inst_t|dma_channel_\w+|pio_sm_\w+)\b", text), source


def test_sdio_diagnostics_depend_on_public_types_not_the_backend_contract():
    header = (SDCARD / "sdio/sd_sdio_internal.h").read_text()
    assert '#include "sd.h"' in header
    assert "sd_backend.h" not in header


def test_pio_program_acquisition_uses_results_instead_of_space_prechecks():
    assert "pio_can_add_program" not in (SDCARD / "sdio/sdio_pio.c").read_text()


def test_common_lifecycle_and_raw_wire_interfaces_do_not_expose_device_transactions():
    common = (SDCARD / "sd_link.h").read_text()
    assert not re.search(r"\b(?:response_bits|response_words|dummy_cycles|address_bytes|tx_prepare)\b", common)
    assert "pf_sd_link_sdio_state_t" not in common
    serial = (SDCARD / "spi/sd_link_spi.h").read_text()
    assert not re.search(r"\b(?:command|address_bytes|dummy_cycles)\s*;", serial)
    native = (SDCARD / "sdio/sd_link_sdio.h").read_text()
    assert not re.search(r"\b(?:clock_owner|last_response_words|rx_ready|mode_restore_failed)\b", native)
    physical = (SDCARD / "sdio/sdio_pio.c").read_text()
    assert "prepare_80_clocks" not in physical


@pytest.mark.parametrize("mode", ["spi", "sdio_pio"])
def test_backend_api_and_guards_match_module_names(mode):
    header = (ENGINES[mode] / f"{mode}.h").read_text()
    guard = f"PF_{mode.upper()}_H"
    assert re.search(rf"^#ifndef {guard}\n#define {guard}$", header, re.MULTILINE)
    assert f"pf_{mode}_t" in header
    assert f"pf_{mode}_bus(" in header
    for source in ENGINES[mode].rglob("*"):
        if source.suffix in (".c", ".h", ".pio"):
            assert not re.search(rf"\bpf_storage_{mode}_(?!pins_)\w+", source.read_text()), source
            assert not re.search(rf"\bPF_STORAGE_{mode.upper()}_H\b", source.read_text()), source


@pytest.mark.parametrize("module", ["sd_link", "sd_link_spi", "sd_link_sdio"])
def test_wire_interface_names_and_guards_match_modules(module):
    directory = SDCARD if module == "sd_link" else ENGINES[
        "spi" if module == "sd_link_spi" else "sdio_pio"
    ]
    header = (directory / f"{module}.h").read_text()
    guard = f"PF_{module.upper()}_H"
    assert re.search(rf"^#ifndef {guard}\n#define {guard}$", header, re.MULTILINE)
    assert f"pf_{module}_" in header
    sources = [directory / f"{module}.h"]
    if module == "sd_link":
        sources.append(SDCARD / "sd_link.c")
    else:
        backend = "spi" if module == "sd_link_spi" else "sdio_pio"
        sources.append(ENGINES[backend] / f"{backend}.c")
        assert not (directory / f"{module}.c").exists()
    for source in sources:
        text = source.read_text()
        assert not re.search(r"\b(?:pf_storage_transport\w*|pf_storage_native\w*|PF_STORAGE_TRANSPORT\w*"
                             r"|PF_STORAGE_NATIVE\w*)\b", text)
    functions = re.findall(r"\b(pf_sd_link_\w+)\s*\(", header)
    backends = ("spi", "sdio_pio") if module == "sd_link" else (backend,)
    for implementation in backends:
        backend_text = (ENGINES[implementation] / f"{implementation}.c").read_text()
        if module == "sd_link":
            backend_text += (SDCARD / "sd_link.c").read_text()
        for function in functions:
            assert re.search(rf"\b{function}\s*\([^;]*?\)\s*\{{", backend_text, re.DOTALL), function


def test_sd_link_uses_static_functions_without_operation_tables_or_serial_phases():
    for source in PHYSICAL_SOURCES:
        assert not re.search(
            r"\b(?:pf_sd_link(?:_spi|_sdio)?_ops_t|pf_sd_link_spi_phase_t"
            r"|pf_sd_link_spi_transfer|pf_spi_transfer)\b",
            source.read_text()
        ), source


def test_protocol_backends_implement_one_shared_contract_and_cmake_selects_board_binding():
    header = (SDCARD / "sd_backend.h").read_text()
    functions = re.findall(r"\b(pf_sd_backend_\w+)\s*\(", header)
    assert functions
    for mode, diagnostic in (("sdio", "sd_sdio_internal"), ("spi", "sd_spi_internal")):
        source = (SDCARD / mode / f"sd_backend_{mode}.c").read_text()
        for function in functions:
            assert re.search(rf"\b{function}\s*\([^;]*?\)\s*\{{", source, re.DOTALL), (mode, function)
        assert not re.search(r"\bpf_sd_backend_\w+\s*\(", (SDCARD / mode / f"{diagnostic}.h").read_text())
    board_header = (SDCARD / "sd_board_internal.h").read_text()
    for mode in ("sdio", "spi"):
        source = (SDCARD / mode / f"sd_board_{mode}.c").read_text()
        for function in re.findall(r"\b(pf_sd_board_transport\w*)\s*\(", board_header):
            assert re.search(rf"\b{function}\s*\([^;]*?\)\s*\{{", source, re.DOTALL), (mode, function)
    facade = (SDCARD / "sd.c").read_text()
    assert '#include "sd_backend.h"' in facade
    assert not re.search(r'#include "(?:sd_sdio_internal|sd_spi_internal)\.h"', facade)
    cmake = (ROOT / "firmware/CMakeLists.txt").read_text()
    assert "PF_CONFIG_SDCARD_TRANSPORT" not in cmake
    sdio, spi = cmake.split('if(PF_SDCARD_TRANSPORT STREQUAL "SDIO_PIO")', 1)[1].split("else()", 1)
    assert "src/drivers/sdcard/sdio/sd_backend_sdio.c" in sdio
    assert "src/drivers/sdcard/sdio/sd_board_sdio.c" in sdio
    spi = spi.split("endif()", 1)[0]
    assert "src/drivers/sdcard/spi/sd_backend_spi.c" in spi
    assert "src/drivers/sdcard/spi/sd_board_spi.c" in spi


def test_transport_implementations_stay_out_of_shared_directories():
    assert not (SDCARD / "link").exists()
    assert sorted(source.name for source in SDCARD.glob("sd_link*")) == ["sd_link.c", "sd_link.h"]
    for source in SDCARD.glob("*.[ch]"):
        assert not re.match(
            r"(?:sd_native|sd_spi|sdio_|sd_backend_(?:sdio|spi)|sd_board_(?:sdio|spi))",
            source.name
        ), source
    for mode in ("sdio", "spi"):
        for contract in ("sd_backend", "sd_board_internal"):
            assert (SDCARD / f"{contract}.h").is_file()
            assert not (SDCARD / mode / f"{contract}.h").exists()


def test_public_sd_api_is_declared_in_one_header():
    assert not (SDCARD / "sd_block.h").exists()
    header = (SDCARD / "sd.h").read_text()
    for function in ("init", "lock", "read_blocks", "write_blocks", "sync", "get_info"):
        assert re.search(rf"\bpf_sd_{function}\s*\(", header), function


def test_sdio_host_and_diagnostics_match_their_implementation_owners():
    host_header = (SDCARD / "sdio/sd_sdio_host.h").read_text()
    host_source = (SDCARD / "sdio/sd_sdio_host.c").read_text()
    diagnostics = (SDCARD / "sdio/sd_sdio_internal.h").read_text()
    backend = (SDCARD / "sdio/sd_backend_sdio.c").read_text()
    for header, implementation in ((host_header, host_source), (diagnostics, backend)):
        functions = set(re.findall(r"\b(pf_sd_sdio_\w+)\s*\(", header))
        assert functions
        for function in functions:
            assert re.search(
                rf"\b{function}\s*\([^;]*?\)\s*\{{", implementation + header, re.DOTALL
            ), function
        assert not re.search(r"\bpf_sd_native_(?:host_|get_|init_stage)", header + implementation)
        assert not re.search(
            r"\bPF_SD_NATIVE_(?:INIT_STAGE_|TRANSFER_CLOCK_HZ|DEFAULT_CLOCK_MAX_HZ|HIGH_SPEED)",
            header + implementation
        )
