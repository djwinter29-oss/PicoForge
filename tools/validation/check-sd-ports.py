#!/usr/bin/env python3
"""Run the bare-metal facade port and optionally inspect cross-build artifacts."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "firmware/src/drivers/sd_card"

HARNESS = r"""
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include "sd_card.h"
#include "sd_card_msc.h"
#include "sd_card_port.h"

static bool initialized;
static unsigned reads, writes, syncs;
static uint8_t media[1024];
pf_sd_card_result_t pf_sdio_pio_init(const pf_sdio_pio_config_t *config)
{ (void)config; initialized = true; return PF_SD_CARD_OK; }
bool pf_sdio_pio_is_initialized(void) { return initialized; }
uint32_t pf_sdio_pio_block_count(void) { return initialized ? 2u : 0u; }
pf_sdio_pio_init_stage_t pf_sdio_pio_get_init_stage(void)
{ return PF_SDIO_PIO_INIT_STAGE_NONE; }
void pf_sdio_pio_get_info(pf_sd_card_info_t *info)
{ *info = (pf_sd_card_info_t){ .initialized = initialized }; }
void pf_sdio_pio_confirm_card_removed(void) { initialized = false; }
pf_sd_card_result_t pf_sdio_pio_read_blocks(uint32_t first, void *buffer, uint32_t count)
{
    assert(initialized && first == 0 && count == 2 && buffer != NULL);
    ++reads; memcpy(buffer, media, sizeof(media)); return PF_SD_CARD_OK;
}
pf_sd_card_result_t pf_sdio_pio_write_blocks(uint32_t first, const void *buffer, uint32_t count)
{
    assert(initialized && first == 0 && count == 2 && buffer != NULL);
    ++writes; memcpy(media, buffer, sizeof(media)); return PF_SD_CARD_OK;
}
pf_sd_card_result_t pf_sdio_pio_sync(void) { ++syncs; return PF_SD_CARD_OK; }
void pf_sd_card_get_profile(pf_sd_card_profile_t *profile)
{ *profile = (pf_sd_card_profile_t){0}; }

int main(void)
{
    pf_sd_card_port_init(NULL);
    assert(pf_sd_card_try_lock());
    pf_sd_card_lock();
    assert(pf_sd_card_try_lock());
    pf_sd_card_unlock(); pf_sd_card_unlock(); pf_sd_card_unlock();
    assert(pf_sd_card_init(NULL) == PF_SD_CARD_INVALID_ARGUMENT);
    assert(pf_sd_card_read_blocks(0, NULL, 0) == PF_SD_CARD_NOT_CONFIGURED);
    pf_sd_card_config_t config = { .transport = PF_SD_CARD_TRANSPORT_SDIO_PIO };
    pf_sd_card_port_init(&config);
    assert(pf_sd_card_init(&config) == PF_SD_CARD_OK);
    uint8_t input[1024], output[1024];
    memset(input, 0x5a, sizeof(input));
    assert(pf_sd_card_write_blocks(0, input, 2) == PF_SD_CARD_OK);
    assert(pf_sd_card_sync() == PF_SD_CARD_OK);
    assert(pf_sd_card_read_blocks(0, output, 2) == PF_SD_CARD_OK);
    assert(memcmp(input, output, sizeof(input)) == 0);
    assert(pf_sd_card_msc_acquire() == PF_SD_CARD_OK);
    assert(pf_sd_card_write_blocks(0, input, 2) == PF_SD_CARD_NOT_READY);
    assert(writes == 1);
    assert(pf_sd_card_msc_write_blocks(0, input, 2) == PF_SD_CARD_OK);
    assert(pf_sd_card_msc_read_blocks(0, output, 2) == PF_SD_CARD_OK);
    assert(pf_sd_card_msc_sync() == PF_SD_CARD_OK);
    assert(reads == 2 && writes == 2 && syncs == 2);
    assert(pf_sd_card_msc_release() == PF_SD_CARD_OK);
    assert(pf_sd_card_take_reinit_request());
    pf_sd_card_info_t info;
    pf_sd_card_get_info(&info);
    assert(info.initialized && pf_sd_card_block_count() == 2);
}
"""


def check_host():
    with tempfile.TemporaryDirectory() as directory:
        scratch = Path(directory)
        for name, body in {
            "hardware/pio.h": "#include <stdbool.h>\ntypedef void *PIO;\n",
            "hardware/spi.h": "typedef struct spi_inst spi_inst_t;\n",
        }.items():
            header = scratch / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text(body)
        for transport in (0, 1):
            code = HARNESS
            if transport == 1:
                code = code.replace("pf_sdio_pio_", "pf_sd_spi_")
                code = code.replace("PF_SDIO_PIO_INIT_STAGE_NONE", "PF_SD_SPI_INIT_STAGE_NONE")
                code = code.replace("PF_SD_CARD_TRANSPORT_SDIO_PIO", "PF_SD_CARD_TRANSPORT_SPI")
                code = code.replace(
                    "void pf_sd_spi_confirm_card_removed(void) { initialized = false; }", "")
            source = scratch / "test.c"
            exe = scratch / "test"
            source.write_text(code)
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                f"-DPF_CONFIG_SD_CARD_TRANSPORT={transport}",
                "-I" + directory, "-I" + str(ROOT / "firmware/src"), "-I" + str(DRIVER),
                str(DRIVER / "sd_card.c"), str(DRIVER / "ports/baremetal/sd_card_port.c"),
                str(source), "-o", str(exe)
            ], check=True)
            subprocess.run([str(exe)], check=True, timeout=10)
    print("Bare-metal facade: recursive no-op reservations, bytes, ownership and sync passed (SDIO/SPI)")


def check_build(directory):
    directory = directory.resolve()
    commands = json.loads((directory / "compile_commands.json").read_text())
    files = [item["file"] for item in commands]
    ports = [file for file in files if "/drivers/sd_card/ports/" in file]
    expected = str(DRIVER / "ports/baremetal/sd_card_port.c")
    if ports != [expected]:
        raise AssertionError(f"Expected only bare-metal port in {directory}: {ports}")
    for item in commands:
        if "FreeRTOS" in item["command"] or "FreeRTOS" in item["file"]:
            raise AssertionError(f"Kernel dependency in {directory}: {item['file']}")
    elf = directory / "pf_sd_card_baremetal_smoke.elf"
    symbols = subprocess.check_output(
        [os.environ.get("ARM_NM", "arm-none-eabi-nm"), str(elf)], text=True)
    if re.search(r"\b(?:[xv]Task\w*|[xv]Queue\w*|[xv]Port\w*|pvPort\w*|vApplication\w*)$", symbols,
                 re.MULTILINE):
        raise AssertionError(f"FreeRTOS symbols in {elf}")
    for symbol in ("pf_sd_card_init", "pf_sd_card_read_blocks", "pf_sd_card_write_blocks",
                   "pf_sd_card_sync", "pf_sd_card_port_try_lock"):
        if not re.search(r"\b" + symbol + r"$", symbols, re.MULTILINE):
            raise AssertionError(f"Missing {symbol} in {elf}")
    print(f"Bare-metal compile/link: exactly one port, no kernel includes/symbols: {elf}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", action="append", type=Path, default=[])
    args = parser.parse_args()
    check_host()
    for directory in args.build_dir:
        check_build(directory)


if __name__ == "__main__":
    main()
