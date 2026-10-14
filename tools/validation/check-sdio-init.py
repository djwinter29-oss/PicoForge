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
#include <string.h>
#define PF_SD_CARD_BLOCK_SIZE 512u
#define PF_SD_CARD_OK 0
#define PF_SD_CARD_INIT_FAILED 1
#define PF_SDIO_PIO_INIT_STAGE_TRANSFER_CLOCK 1
#define PF_SDIO_PIO_INIT_STAGE_NONE 0
static struct {
    uint32_t current_clock_hz;
    bool data_rx_multiblock_supported;
} sdio_pio_host;
static struct {
    uint32_t clock_hz;
    bool use_card_detect;
    unsigned card_detect_gpio;
    bool card_detect_active_high;
} active_config = { .clock_hz = 50000000u };
static uint32_t card_sector_count = 15564800u;
static uint8_t card_cid[16];
static bool card_block_addressed = true, card_initialized;
static unsigned card_init_stage, attempts, resets, reads, sectors, high_speed_requests;
static uint32_t system_hz = 150000000u;
static bool corrupt_fast, fail_reads, lose_batching, fail_identification;
static bool runtime_read;
static bool change_cid, change_capacity;
static uint32_t fail_above_hz;
static bool pf_sdio_pio_is_initialized(void) { return card_initialized; }
static unsigned gpio_get(unsigned pin) { (void)pin; return 1u; }
static bool initialize_card(uint32_t *clock_limit_hz);
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
    assert(addressed && count <= 64u);
    if (!runtime_read) assert(first == sectors);
    ++reads;
    sectors += count;
    if (corrupt_fast && sdio_pio_host.current_clock_hz > 25000000u)
        sdio_pio_host.current_clock_hz = 1000000u;
    if (lose_batching) sdio_pio_host.data_rx_multiblock_supported = false;
    return !fail_reads && (!fail_above_hz || sdio_pio_host.current_clock_hz <= fail_above_hz);
}
static uint32_t default_clock_hz(void) { return 10000000u; }
@LADDER@
@PROBE@
@READ_RETRY@
static bool initialize_card(uint32_t *clock_limit_hz)
{
    ++attempts;
    sectors = 0u;
    if (fail_identification) { card_init_stage = 99u; return false; }
    if (change_cid) ++card_cid[0];
    if (change_capacity) ++card_sector_count;
    @REQUEST@
    uint32_t transfer_clock = requested_clock;
    if (requested_clock > 25000000u) ++high_speed_requests;
    @SELECT@
}
static int initialize(void)
{
    @RETRY@
}
static void reset(void)
{
    attempts = resets = reads = sectors = 0u;
    high_speed_requests = 0u;
    card_init_stage = 0u;
    corrupt_fast = fail_reads = lose_batching = fail_identification = false;
    runtime_read = false;
    change_cid = change_capacity = false;
    memset(card_cid, 0, sizeof(card_cid));
    active_config.use_card_detect = false;
    fail_above_hz = 0u;
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
    reset();
    runtime_read = card_initialized = true;
    sdio_pio_host.current_clock_hz = 33000000u;
    fail_above_hz = 20000000u;
    uint8_t buffer[512];
    assert(read_blocks_with_clock_fallback(4096u, buffer, 1u));
    assert(high_speed_requests == 0u);
    assert(reads == 7u && attempts == 2u && resets == 2u && sdio_pio_host.current_clock_hz == 20000000u);
    assert(read_blocks_with_clock_fallback(4097u, buffer, 1u));
    assert(reads == 8u && sdio_pio_host.current_clock_hz == 20000000u);
    fail_reads = true;
    assert(!read_blocks_with_clock_fallback(4098u, buffer, 1u));
    assert(!card_initialized && reads == 12u);
    card_initialized = false;
    assert(!read_blocks_with_clock_fallback(4098u, buffer, 1u));
    assert(reads == 12u);
    reset();
    runtime_read = card_initialized = change_cid = true;
    sdio_pio_host.current_clock_hz = 33000000u;
    fail_above_hz = 25000000u;
    assert(!read_blocks_with_clock_fallback(4096u, buffer, 1u));
    assert(!card_initialized && attempts == 1u && reads == 5u);
    reset();
    runtime_read = card_initialized = change_capacity = true;
    sdio_pio_host.current_clock_hz = 33000000u;
    fail_above_hz = 25000000u;
    assert(!read_blocks_with_clock_fallback(4096u, buffer, 1u));
    assert(!card_initialized && attempts == 1u && reads == 5u);
    reset();
    runtime_read = card_initialized = true;
    active_config.use_card_detect = true;
    sdio_pio_host.current_clock_hz = 33000000u;
    fail_reads = true;
    assert(!read_blocks_with_clock_fallback(4096u, buffer, 1u));
    assert(!card_initialized && attempts == 0u && reads == 1u);
    puts("SDIO initialization checks passed");
}
"""


def main():
    source = SOURCE.read_text()
    probe = function_source(source, "static bool probe_transfer(void)")
    read_retry = function_source(source, "static bool read_blocks_with_clock_fallback(")
    init = function_source(source, "static bool initialize_card(")
    request = init[init.index("    uint32_t requested_clock"):init.index("    uint32_t transfer_clock")]
    retry = function_source(source, "pf_sd_card_result_t pf_sdio_pio_init(")
    ladder_start = source.index("static const uint32_t transfer_clock_steps_hz[]")
    ladder = source[ladder_start:source.index("};", ladder_start) + 2]
    selection = init[init.index("card_init_stage = PF_SDIO_PIO_INIT_STAGE_TRANSFER_CLOCK;"):-1]
    retry = retry[retry.index("card_initialized = false;\n    uint32_t clock_limit_hz"):-1]
    code = (HARNESS.replace("@LADDER@", ladder).replace("@PROBE@", probe)
            .replace("@READ_RETRY@", read_retry)
            .replace("@REQUEST@", request)
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