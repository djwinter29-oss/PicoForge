#!/usr/bin/env python3
"""Source check for the SDIO CMD18 batch-count load (host SDK not required)."""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HOST = ROOT / "firmware/src/drivers/sd_card/sdio_pio/sdio_pio_host.c"
SELF_TEST = ROOT / "firmware/src/middleware/storage/self_test.h"


def function_body(source, name):
    start = source.index(name + "(")
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        depth += {"{": 1, "}": -1}.get(source[index], 0)
        if depth == 0:
            return source[brace:index]
    raise ValueError(name)


HARNESS = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define SDIO_DATA_WORD_COUNT 4u
#define PF_SD_CARD_BLOCK_SIZE 16u
typedef struct { uint32_t write_addr; } dma_hw_t;
typedef struct { unsigned dma_channel; } pf_sdio_pio_host_t;
static dma_hw_t hw;
static uint32_t read_wire_buffer[SDIO_DATA_WORD_COUNT * 4u];
#define BASE ((uint32_t)(uintptr_t)read_wire_buffer)
static int barriers, decodes, fail_at = -1;
static int order[8];
static dma_hw_t *dma_channel_hw_addr(unsigned c) { (void)c; return &hw; }
static void __dmb(void) { ++barriers; }
static bool pf_sdio_pio_decode_data_block(const uint8_t *image, uint8_t *block)
{
    if (decodes == fail_at) { return false; }
    order[decodes++] = (int)((const uint32_t *)image - read_wire_buffer);
    block[0] = (uint8_t)decodes;
    return true;
}
static bool pf_sdio_pio_verify_data_block(const uint8_t *image)
{
    uint8_t discarded[PF_SD_CARD_BLOCK_SIZE];
    return pf_sdio_pio_decode_data_block(image, discarded);
}
@HELPER@
static int run(uint32_t words, uint32_t *decoded, uint8_t *blocks, bool *ok)
{
    pf_sdio_pio_host_t host = {0};
    hw.write_addr = BASE + words * 4u;
    *ok = decode_completed_blocks(&host, blocks, 16u, decoded);
    return 0;
}
static void reset(uint32_t *d) { *d = 0; decodes = 0; fail_at = -1; }
int main(void)
{
    uint8_t blocks[64] = {0};
    uint32_t d = 0;
    bool ok;
    int b;
    run(0u, &d, blocks, &ok);                        /* start boundary */
    if (!ok || d != 0 || decodes) return 1;
    run(3u, &d, blocks, &ok);                        /* partial word block */
    if (!ok || d != 0 || decodes) return 2;
    run(15u, &d, blocks, &ok);                       /* CRC tail pending */
    if (!ok || d != 3 || decodes != 3) return 3;
    reset(&d);
    run(4u, &d, blocks, &ok);                        /* one full block */
    if (!ok || d != 1 || decodes != 1) return 4;
    run(9u, &d, blocks, &ok);                        /* more, partial */
    if (!ok || d != 2 || decodes != 2) return 5;
    run(9u, &d, blocks, &ok);                        /* repeat: exactly once */
    if (!ok || decodes != 2) return 6;
    run(16u, &d, blocks, &ok);                       /* all */
    if (!ok || d != 4 || decodes != 4) return 7;
    for (b = 0; b < 4; ++b) {
        if (order[b] != b * 4 || blocks[b * 16] != b + 1) return 8;
    }
    reset(&d);
    hw.write_addr = BASE - 4u;                       /* underflow */
    if (decode_completed_blocks(&(pf_sdio_pio_host_t){0}, blocks, 16u, &d) || decodes) return 9;
    run(17u, &d, blocks, &ok);                       /* overflow */
    if (ok || d != 0 || decodes) return 10;
    hw.write_addr = BASE + 17u;                      /* unaligned */
    if (decode_completed_blocks(&(pf_sdio_pio_host_t){0}, blocks, 16u, &d) || decodes) return 11;
    fail_at = 1;
    run(16u, &d, blocks, &ok);                       /* CRC failure on block 1 */
    if (ok || d != 1 || decodes != 1) return 12;
    if (barriers < 6) return 13;
    reset(&d);
    run(16u, &d, NULL, &ok);
    if (!ok || d != 4 || decodes != 4) return 14;
    reset(&d);
    fail_at = 1;
    run(16u, &d, NULL, &ok);
    if (ok || d != 1 || decodes != 1) return 15;
    return 0;
}
"""


def helper_source(source):
    start = source.index("static bool decode_completed_blocks")
    return source[start:start + source[start:].index("\n}\n") + 3]


READER_HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "sd_card_protocol.h"
#define SDIO_DATA_READ_IRQ 1u
#define SDIO_DATA_NIBBLE_COUNT 1040u
#define SDIO_DATA_WORD_COUNT 130u
#define SDIO_READ_BATCH_BLOCKS 64u
typedef struct { uint32_t rxf[4]; } fake_pio_t;
typedef struct {
    fake_pio_t *pio;
    unsigned data_sm, dma_channel, data_rx_program_offset;
    bool data_rx_program_loaded, data_rx_multiblock_supported;
} pf_sdio_pio_host_t;
static uint32_t read_wire_buffer[130u * 64u];
static bool response_ok, crc_ok, stop_ok, timeout, command_ok;
static uint32_t response_status;
static unsigned single_calls, stops;
static uint32_t now;
static uint32_t rx_transfer_words, rx_nibble_count, rx_block_count;
#define pio_interrupt_get(...) (!timeout)
#define dma_channel_is_busy(...) timeout
static uint32_t time_us_32(void) { now += 1000001u; return now; }
static void start_data_rx(pf_sdio_pio_host_t *host, uint32_t *destination,
                          uint32_t transfer_words, uint32_t nibble_count,
                          uint32_t block_count)
{
    (void)host;
    assert(destination == read_wire_buffer);
    rx_transfer_words = transfer_words;
    rx_nibble_count = nibble_count;
    rx_block_count = block_count;
}
static void cleanup_data_rx(pf_sdio_pio_host_t *host) { (void)host; }
static void wait_data_idle(pf_sdio_pio_host_t *host, uint32_t completion_flag)
{ (void)host; (void)completion_flag; }
static bool pf_sdio_pio_host_command_bytes(pf_sdio_pio_host_t *host, uint8_t command,
    uint32_t argument, unsigned bits, uint8_t *response, unsigned size)
{ (void)host; (void)command; (void)argument; (void)bits; (void)response; (void)size; return command_ok; }
static bool pf_sdio_pio_parse_response(uint8_t command, const uint8_t *response,
                                      uint32_t *argument, bool crc)
{ (void)command; (void)response; (void)crc; *argument = response_status; return response_ok; }
static bool decode_completed_blocks(pf_sdio_pio_host_t *host, uint8_t *blocks,
                                     uint32_t words, uint32_t *decoded)
{ (void)host; (void)blocks; *decoded = words / SDIO_DATA_WORD_COUNT; return crc_ok; }
static bool stop_multiblock_with_clock_fallback(pf_sdio_pio_host_t *host)
{ (void)host; ++stops; return stop_ok; }
static bool read_single_blocks(pf_sdio_pio_host_t *host, uint32_t sector,
                               bool addressed, uint8_t *blocks, uint32_t count)
{ (void)host; (void)sector; (void)addressed; (void)blocks; (void)count; ++single_calls; return true; }
@READER@
int main(void)
{
    fake_pio_t pio = {0};
    pf_sdio_pio_host_t host = { .pio = &pio, .data_rx_program_loaded = true,
                               .data_rx_multiblock_supported = true };
    uint8_t blocks[1024];
    response_ok = crc_ok = stop_ok = command_ok = true;
    assert(pf_sdio_pio_host_read_blocks(&host, 0u, true, blocks, 2u));
        assert(rx_transfer_words == 2u * SDIO_DATA_WORD_COUNT &&
            rx_nibble_count == SDIO_DATA_NIBBLE_COUNT && rx_block_count == 2u);
    assert(stops == 1u && single_calls == 0u);
    crc_ok = false;
    assert(!pf_sdio_pio_host_read_blocks(&host, 0u, true, blocks, 2u));
    assert(host.data_rx_multiblock_supported && single_calls == 0u);
    crc_ok = true;
    timeout = true;
    assert(!pf_sdio_pio_host_read_blocks(&host, 0u, true, blocks, 2u));
    assert(host.data_rx_multiblock_supported && single_calls == 0u);
    timeout = false;
    stop_ok = false;
    assert(!pf_sdio_pio_host_read_blocks(&host, 0u, true, blocks, 2u));
    assert(host.data_rx_multiblock_supported && single_calls == 0u);
    stop_ok = true;
    assert(pf_sdio_pio_host_read_blocks(&host, 0u, true, NULL, 2u));
    command_ok = false;
    assert(!pf_sdio_pio_host_read_blocks(&host, 0u, true, blocks, 2u));
    assert(host.data_rx_multiblock_supported && single_calls == 0u);
    command_ok = true;
    response_ok = false;
    assert(!pf_sdio_pio_host_read_blocks(&host, 0u, true, blocks, 2u));
    assert(host.data_rx_multiblock_supported && single_calls == 0u);
    response_ok = true;
    response_status = 1u << 22u;
    assert(pf_sdio_pio_host_read_blocks(&host, 0u, true, blocks, 2u));
    assert(!host.data_rx_multiblock_supported && single_calls == 1u);
    assert(pf_sdio_pio_host_read_blocks(&host, 2u, true, blocks, 2u));
    assert(single_calls == 2u);
    return 0;
}
"""


def reader_behavior_check(source):
    signature = "bool pf_sdio_pio_host_read_blocks"
    start = source.index(signature)
    reader = source[start:source.index("{", start)] + function_body(source, signature) + "}"
    with tempfile.TemporaryDirectory() as scratch:
        src = Path(scratch) / "reader.c"
        exe = Path(scratch) / "reader"
        src.write_text(READER_HARNESS.replace("@READER@", reader))
        include = HOST.parents[1]
        build = subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                                "-I", str(include), str(src),
                                str(include / "sd_card_protocol.c"), "-o", str(exe)],
                               capture_output=True, text=True)
        if build.returncode:
            return ["reader harness compile failed: " + build.stderr]
        result = subprocess.run([str(exe)])
        return [] if result.returncode == 0 else ["reader recovery behavior failed"]


def behavior_check(source):
    cc = os.environ.get("CC", "cc")
    code = HARNESS.replace("@HELPER@", helper_source(source))
    with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as scratch:
        src = os.path.join(scratch, "harness.c")
        exe = os.path.join(scratch, "harness")
        Path(src).write_text(code)
        build = subprocess.run([cc, "-Wall", "-Werror", "-o", exe, src],
                               capture_output=True, text=True)
        if build.returncode:
            return ["helper harness failed to compile: " + build.stderr]
        result = subprocess.run([exe])
        if result.returncode:
            return ["decode helper behavior check failed, step %d" % result.returncode]
    return []


def main():
    source = HOST.read_text()
    errors = behavior_check(source) + reader_behavior_check(source)

    batch = int(re.search(r"#define SDIO_READ_BATCH_BLOCKS (\d+)u", source).group(1))
    buffer_size = int(re.search(r"SELF_TEST_BUFFER_SIZE (\d+)u", SELF_TEST.read_text()).group(1))
    if batch * 512 > buffer_size or buffer_size % 512:
        errors.append("batch ceiling must cover the self-test buffer")

    for value in re.findall(r"pio_encode_set\(\s*pio_y,\s*([^)]*)\)", source):
        value = value.strip().rstrip("uU")
        if not value.isdigit() or int(value) > 31:
            errors.append("SET y operand must be a literal <= 31: " + value)

    loader = function_body(source, "static void load_batch_count")
    order = [loader.find(token) for token in (
        "pio_sm_put_blocking", "pio_encode_pull", "pio_encode_mov(pio_y, pio_osr)")]
    if min(order) < 0 or order != sorted(order):
        errors.append("batch count must be put, pulled, then moved to y")

    reader = function_body(source, "bool pf_sdio_pio_host_read_blocks")
    positions = [reader.find(token) for token in (
        "start_data_rx(host, read_wire_buffer", "command_bytes(host, 18u",
        "while (!pio_interrupt_get", "cleanup_data_rx(host)",
        "stop_multiblock_with_clock_fallback(host)")]
    if min(positions) < 0 or positions != sorted(positions):
        errors.append("batch RX setup, command, overlap wait, cleanup, and stop order is invalid")
    rx_start = function_body(source, "static void start_data_rx")
    start_order = [rx_start.find(token) for token in (
        "pio_encode_jmp(host->data_rx_program_offset)",
        "load_batch_count(host, block_count)", "nibble_count - 1u",
        "dma_channel_configure", "dma_channel_start",
        "pio_sm_set_enabled(host->pio, host->data_sm, true)")]
    if min(start_order) < 0 or start_order != sorted(start_order):
        errors.append("RX helper must load framing/count, configure DMA, start DMA, then enable PIO")
    rx_cleanup = function_body(source, "static void cleanup_data_rx")
    cleanup_order = [rx_cleanup.find(token) for token in (
        "pio_sm_set_enabled", "dma_channel_abort", "pio_sm_clear_fifos",
        "pio_interrupt_clear", "pio_sm_restart")]
    if min(cleanup_order) < 0 or cleanup_order != sorted(cleanup_order):
        errors.append("RX helper cleanup order is invalid")

    helper = function_body(source, "static bool decode_completed_blocks")
    for token in ("write_addr", "buffer_start", "__dmb()",
                  "/ SDIO_DATA_WORD_COUNT", "pf_sdio_pio_decode_data_block"):
        if token not in helper:
            errors.append("decode helper missing: " + token)
    if "transfer_count" in helper:
        errors.append("decode helper must not rely on TRANS_COUNT")
    if reader.count("pf_sdio_pio_decode_data_block") or "block_index" in reader:
        errors.append("read_blocks must decode only via the overlap helper")
    wait = reader.find("while (!pio_interrupt_get")
    if not 0 < reader.find("decode_completed_blocks", wait) < reader.find("stop_multiblock_with_clock_fallback(host)"):
        errors.append("decode must overlap the DMA wait before CMD12")
    if "decoded_count != batch_count" not in reader or "crc_failed" not in reader:
        errors.append("final decode must verify all blocks decoded")

    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("SDIO read batch check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
