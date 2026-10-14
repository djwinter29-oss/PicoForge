#!/usr/bin/env python3
"""Exercise SDIO qualification and retry control flow without a Pico SDK."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "firmware/src/drivers/sd_card/sdio_pio/sdio_pio.c"


def function_source(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        depth += {"{": 1, "}": -1}.get(source[index], 0)
        if depth == 0:
            return source[start:index + 1]
    raise ValueError(signature)


HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#define PF_SD_CARD_BLOCK_SIZE 512u
#define PF_SD_CARD_OK 0
#define PF_SD_CARD_INIT_FAILED 1
#define PF_SDIO_PIO_INIT_STAGE_TRANSFER_CLOCK 1
#define PF_SDIO_PIO_INIT_STAGE_NONE 0
static struct {
    uint32_t current_clock_hz;
    bool data_rx_multiblock_supported;
} sdio_pio_host;
static struct { uint32_t clock_hz; } active_config = {50000000u};
static uint32_t card_sector_count = 15564800u;
static bool card_block_addressed = true, card_initialized;
static unsigned card_init_stage, attempts, resets, reads, sectors;
static uint32_t system_hz = 150000000u;
static bool corrupt_fast, fail_reads, lose_batching, fail_identification;
static bool pf_sdio_pio_host_set_clock(void *host, uint32_t hz)
{
    (void)host;
    if (hz > system_hz / (hz > 25000000u ? 4u : 2u)) return false;
    sdio_pio_host.current_clock_hz = hz;
    if (hz == 400000u) ++resets;
    return true;
}
static bool pf_sdio_pio_host_read_blocks(void *host, uint32_t first,
                                        bool addressed, uint8_t *data, uint32_t count)
{
    (void)host; (void)data;
    assert(addressed && first == sectors && count <= 64u);
    ++reads;
    sectors += count;
    if (corrupt_fast && sdio_pio_host.current_clock_hz > 25000000u)
        sdio_pio_host.current_clock_hz = 1000000u;
    if (lose_batching) sdio_pio_host.data_rx_multiblock_supported = false;
    return !fail_reads;
}
static uint32_t default_clock_hz(void) { return 10000000u; }
@LADDER@
@PROBE@
static bool initialize_card(uint32_t *clock_limit_hz)
{
    ++attempts;
    sectors = 0u;
    if (fail_identification) { card_init_stage = 99u; return false; }
    uint32_t transfer_clock = active_config.clock_hz;
    if (transfer_clock == 0u) transfer_clock = default_clock_hz();
    if (transfer_clock > *clock_limit_hz) transfer_clock = *clock_limit_hz;
    @SELECT@
}
static int initialize(void)
{
    @RETRY@
}
static void reset(void)
{
    attempts = resets = reads = sectors = 0u;
    card_init_stage = 0u;
    corrupt_fast = fail_reads = lose_batching = fail_identification = false;
    active_config.clock_hz = 50000000u;
    card_sector_count = 15564800u;
    system_hz = 150000000u;
}
int main(void)
{
    reset();
    corrupt_fast = true;
    assert(initialize() == PF_SD_CARD_OK && card_initialized);
    assert(attempts == 2u && resets == 2u && sdio_pio_host.current_clock_hz == 25000000u);
    assert(reads == 5u && sectors == 256u && sdio_pio_host.data_rx_multiblock_supported);
    reset();
    system_hz = 250000000u;
    assert(initialize() == PF_SD_CARD_OK && sdio_pio_host.current_clock_hz == 50000000u);
    assert(attempts == 1u && reads == 4u);
    reset();
    fail_reads = true;
    assert(initialize() == PF_SD_CARD_INIT_FAILED && !card_initialized);
    assert(attempts == 7u && reads == 6u && resets == 7u);
    reset();
    lose_batching = true;
    assert(initialize() == PF_SD_CARD_OK && sdio_pio_host.current_clock_hz == 20000000u);
    assert(attempts == 3u);
    reset();
    fail_identification = true;
    assert(initialize() == PF_SD_CARD_INIT_FAILED && attempts == 1u);
    reset();
    card_sector_count = 3u;
    assert(initialize() == PF_SD_CARD_OK && sectors == 3u && reads == 1u);
    reset();
    active_config.clock_hz = 0u;
    assert(initialize() == PF_SD_CARD_OK && sdio_pio_host.current_clock_hz == 10000000u);
    puts("SDIO initialization checks passed");
}
"""


def main():
    source = SOURCE.read_text()
    probe = function_source(source, "static bool probe_transfer(void)")
    init = function_source(source, "static bool initialize_card(")
    retry = function_source(source, "pf_sd_card_result_t pf_sdio_pio_init(")
    ladder_start = source.index("static const uint32_t transfer_clock_steps_hz[]")
    ladder = source[ladder_start:source.index("};", ladder_start) + 2]
    selection = init[init.index("card_init_stage = PF_SDIO_PIO_INIT_STAGE_TRANSFER_CLOCK;"):-1]
    retry = retry[retry.index("card_initialized = false;\n    uint32_t clock_limit_hz"):-1]
    code = (HARNESS.replace("@LADDER@", ladder).replace("@PROBE@", probe)
            .replace("@SELECT@", selection).replace("@RETRY@", retry))
    with tempfile.TemporaryDirectory() as scratch:
        src = Path(scratch) / "check.c"
        exe = Path(scratch) / "check"
        src.write_text(code)
        subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                        str(src), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()