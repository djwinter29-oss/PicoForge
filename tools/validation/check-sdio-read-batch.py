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
    return 0;
}
"""


def helper_source(source):
    start = source.index("static bool decode_completed_blocks")
    return source[start:start + source[start:].index("\n}\n") + 3]


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
    errors = behavior_check(source)

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
        "pio_encode_jmp(host->data_rx_program_offset)",
        "load_batch_count(host, batch_count)",
        "SDIO_DATA_NIBBLE_COUNT - 1u",
        "pio_sm_set_enabled(host->pio, host->data_sm, true)")]
    if min(positions) < 0 or positions != sorted(positions):
        errors.append("order must be jmp, batch count, nibble count, enable")
    if "pio_encode_set" in reader:
        errors.append("read_blocks must not use SET for the batch count")

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
