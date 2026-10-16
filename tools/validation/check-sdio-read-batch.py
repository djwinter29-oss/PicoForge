#!/usr/bin/env python3
"""Static checks for the SDIO batch reader's PIO and DMA sequencing."""
import re
import sys
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


def ordered(body, tokens, description):
    positions = [body.find(token) for token in tokens]
    if min(positions) < 0 or positions != sorted(positions):
        raise ValueError(description)


def main():
    source = HOST.read_text()
    batch = int(re.search(r"#define SDIO_READ_BATCH_BLOCKS (\d+)u", source).group(1))
    buffer_size = int(re.search(r"SELF_TEST_BUFFER_SIZE (\d+)u", SELF_TEST.read_text()).group(1))
    if batch * 512 > buffer_size or buffer_size % 512:
        raise ValueError("batch ceiling must fit the self-test buffer on sector boundaries")

    for value in re.findall(r"pio_encode_set\(\s*pio_y,\s*([^)]*)\)", source):
        operand = value.strip().rstrip("uU")
        if not operand.isdigit() or int(operand) > 31:
            raise ValueError("SET y operand must be a literal <= 31: " + value)

    loader = function_body(source, "static void load_batch_count")
    ordered(loader, ("pio_sm_put_blocking", "pio_encode_pull", "pio_encode_mov(pio_y, pio_osr)"),
            "batch count must be put, pulled, then moved to y")

    reader = function_body(source, "bool pf_sdio_pio_host_read_blocks")
    ordered(reader, ("start_data_rx(host, read_wire_buffer", "command_bytes(host, 18u",
                    "while (!pio_interrupt_get", "cleanup_data_rx(host)",
                    "stop_multiblock_with_clock_fallback(host)"),
            "batch RX setup, command, overlap wait, cleanup, and stop order is invalid")

    rx_start = function_body(source, "static void start_data_rx")
    ordered(rx_start, ("pio_encode_jmp(host->data_rx_program_offset)",
                       "load_batch_count(host, block_count)", "nibble_count - 1u",
                       "dma_channel_configure", "dma_channel_start",
                       "pio_sm_set_enabled(host->pio, host->data_sm, true)"),
            "RX framing/count, DMA setup, and PIO enable order is invalid")

    cleanup = function_body(source, "static void cleanup_data_rx")
    ordered(cleanup, ("pio_sm_set_enabled", "dma_channel_abort", "pio_sm_clear_fifos",
                      "pio_interrupt_clear", "pio_sm_restart"),
            "RX helper cleanup order is invalid")

    decoder = function_body(source, "static bool decode_completed_blocks")
    for token in ("write_addr", "buffer_start", "__dmb()",
                  "/ SDIO_DATA_WORD_COUNT", "pf_sdio_pio_decode_data_block"):
        if token not in decoder:
            raise ValueError("decode helper missing: " + token)
    if "transfer_count" in decoder:
        raise ValueError("decode helper must use completed DMA write address, not TRANS_COUNT")
    if reader.count("pf_sdio_pio_decode_data_block") or "block_index" in reader:
        raise ValueError("read_blocks must decode only via the overlap helper")
    wait = reader.find("while (!pio_interrupt_get")
    if not 0 < reader.find("decode_completed_blocks", wait) < reader.find(
            "stop_multiblock_with_clock_fallback(host)"):
        raise ValueError("decode must overlap DMA wait before CMD12")
    if "decoded_count != batch_count" not in reader or "crc_failed" not in reader:
        raise ValueError("final decode must verify all blocks decoded")

    print("SDIO batch structural checks passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, AttributeError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
