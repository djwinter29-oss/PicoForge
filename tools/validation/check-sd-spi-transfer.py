#!/usr/bin/env python3
"""Exercise SPI multi-sector transaction control flow without a Pico SDK."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "firmware/src/drivers/sd_card/spi/sd_spi.c"


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
#include <stdint.h>
#include <string.h>
#include "sd_card_protocol.h"
#define SD_SPI_WRITE_TIMEOUT_US 500000u
#define SD_SPI_COMMAND_RETRIES 10u
#define SD_CMD_READ_MULTIPLE_BLOCK 18u
#define SD_CMD_WRITE_MULTIPLE_BLOCK 25u
#define SD_CMD_STOP_TRANSMISSION 12u
#define SD_DATA_START_MULTIPLE 0xfcu
#define SD_DATA_STOP_MULTIPLE 0xfdu
#define SD_R1_ILLEGAL_COMMAND 0x04u
static bool card_initialized = true, card_block_addressed = true;
static bool selected, present = true, ready = true;
static unsigned selects, deselects, read_calls, write_calls, single_calls, stops;
static unsigned commands[8], command_count, fail_at = UINT32_MAX;
static uint8_t command_response, stop_response, last_init_command, last_init_response;
static bool last_init_response_received;
static bool stuff_test;
static unsigned stuff_bytes;
static const uint8_t *expected_base;
static struct { void *spi; } active_config;
static bool card_is_present(void) { return present; }
static bool wait_ready(uint32_t timeout) { (void)timeout; return ready; }
static void select_card(void) { assert(!selected); selected = true; ++selects; }
static void deselect_card(void) { assert(selected); selected = false; ++deselects; }
static uint8_t exchange_byte(uint8_t value)
{
    if (stuff_test) return stuff_bytes++ == 0u ? 0x00u : 0x04u;
    assert(selected && value == SD_DATA_STOP_MULTIPLE);
    ++stops;
    return 0xffu;
}
static bool exchange_byte_checked(uint8_t value, uint8_t *received)
{
    *received = exchange_byte(value);
    return true;
}
static int spi_write_blocking(void *spi, const uint8_t *frame, unsigned size)
{ (void)spi; assert(size == 6u && frame[0] == 0x4cu); return (int)size; }
@COMMAND@
static bool send_command_selected(uint8_t command, uint32_t argument, uint8_t *response)
{
    assert(selected);
    commands[command_count++] = command;
    if (command == SD_CMD_STOP_TRANSMISSION) {
        ++stops;
        *response = stop_response;
    } else {
        assert(argument == 5u);
        *response = command_response;
    }
    return true;
}
static bool read_data_block(uint8_t *buffer, size_t size)
{
    assert(selected && size == 512u && buffer == expected_base + read_calls * 512u);
    return read_calls++ != fail_at;
}
static bool write_data_block(const uint8_t *buffer, uint8_t token)
{
    assert(selected && token == SD_DATA_START_MULTIPLE);
    assert(buffer == expected_base + write_calls * 512u);
    return write_calls++ != fail_at;
}
static pf_sd_card_result_t transfer_block(uint32_t sector, uint8_t *read_buffer,
                                          const uint8_t *write_buffer)
{
    assert(!selected && sector == 5u + single_calls);
    assert((read_buffer != NULL) != (write_buffer != NULL));
    ++single_calls;
    return PF_SD_CARD_OK;
}
static pf_sd_card_result_t validate_transfer(uint32_t first, const void *buffer,
                                             uint32_t count)
{
    return card_initialized ? pf_sd_card_validate_transfer(first, buffer, count, 128u)
                            : PF_SD_CARD_NOT_READY;
}
@TRANSFER@
static void reset(void)
{
    selected = false;
    card_initialized = present = ready = true;
    selects = deselects = read_calls = write_calls = single_calls = stops = command_count = 0u;
    fail_at = UINT32_MAX;
    command_response = stop_response = 0u;
}
int main(void)
{
    uint8_t buffer[1536];
    expected_base = buffer;
    stuff_test = true;
    uint8_t response;
    assert(real_send_command_selected(12u, 0u, &response));
    assert(response == 0x04u && stuff_bytes == 2u && last_init_response_received);
    stuff_test = false;
    reset();
    assert(transfer_blocks(5u, NULL, NULL, 0u) == PF_SD_CARD_OK && selects == 0u);
    assert(transfer_blocks(5u, NULL, NULL, 2u) == PF_SD_CARD_INVALID_ARGUMENT);
    assert(transfer_blocks(127u, buffer, NULL, 2u) == PF_SD_CARD_INVALID_ARGUMENT);
    assert(selects == 0u);
    assert(transfer_blocks(5u, buffer, NULL, 3u) == PF_SD_CARD_OK);
    assert(read_calls == 3u && stops == 1u && selects == 1u && deselects == 1u);
    assert(commands[0] == 18u && commands[1] == 12u && single_calls == 0u);
    reset();
    assert(transfer_blocks(5u, NULL, buffer, 3u) == PF_SD_CARD_OK);
    assert(write_calls == 3u && stops == 1u && command_count == 1u && commands[0] == 25u);
    reset();
    command_response = 0x04u;
    assert(transfer_blocks(5u, buffer, NULL, 3u) == PF_SD_CARD_OK && single_calls == 3u);
    reset();
    command_response = 0x08u;
    assert(transfer_blocks(5u, NULL, buffer, 3u) == PF_SD_CARD_IO_ERROR);
    assert(single_calls == 0u && write_calls == 0u && !selected);
    reset();
    fail_at = 1u;
    assert(transfer_blocks(5u, buffer, NULL, 3u) == PF_SD_CARD_IO_ERROR);
    assert(read_calls == 2u && stops == 1u && single_calls == 0u && !selected);
    reset();
    fail_at = 1u;
    assert(transfer_blocks(5u, NULL, buffer, 3u) == PF_SD_CARD_IO_ERROR);
    assert(write_calls == 2u && stops == 1u && single_calls == 0u && !selected);
    reset();
    stop_response = 1u;
    assert(transfer_blocks(5u, buffer, NULL, 3u) == PF_SD_CARD_IO_ERROR);
    assert(!card_initialized && !selected);
    reset();
    ready = false;
    assert(transfer_blocks(5u, NULL, buffer, 3u) == PF_SD_CARD_IO_ERROR);
    assert(!card_initialized && write_calls == 0u && !selected);
    reset();
    present = false;
    assert(transfer_blocks(5u, NULL, buffer, 3u) == PF_SD_CARD_IO_ERROR);
    assert(!card_initialized && write_calls == 0u && !selected);
    return 0;
}
"""

SINGLE_HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "sd_card_protocol.h"
#include "sd_spi_protocol.h"
#define SD_SPI_WRITE_TIMEOUT_US 500000u
#define SD_CMD_READ_SINGLE_BLOCK 17u
#define SD_CMD_WRITE_SINGLE_BLOCK 24u
#define SD_DATA_START_BLOCK 0xfeu
static bool card_initialized, card_block_addressed = true, selected, present, ready;
static bool command_ok, data_ok;
static uint8_t command_response;
static unsigned data_calls;
static bool card_is_present(void) { return present; }
static bool wait_ready(uint32_t timeout) { (void)timeout; return ready; }
static void select_card(void) { assert(!selected); selected = true; }
static void deselect_card(void) { assert(selected); selected = false; }
static bool send_command_selected(uint8_t command, uint32_t argument, uint8_t *response)
{
    assert(selected && argument == 5u);
    assert(command == SD_CMD_READ_SINGLE_BLOCK || command == SD_CMD_WRITE_SINGLE_BLOCK);
    *response = command_response;
    return command_ok;
}
static bool read_data_block(uint8_t *buffer, size_t size)
{ (void)buffer; assert(selected && size == 512u); ++data_calls; return data_ok; }
static bool write_data_block(const uint8_t *buffer, uint8_t token)
{ (void)buffer; assert(selected && token == 0xfeu); ++data_calls; return data_ok; }
@BLOCK@
static void reset(void)
{
    selected = false;
    card_initialized = present = ready = command_ok = data_ok = true;
    command_response = 0u;
    data_calls = 0u;
}
int main(void)
{
    uint8_t buffer[512];
    for (unsigned write = 0u; write < 2u; ++write) {
        uint8_t *rd = write ? NULL : buffer;
        const uint8_t *wr = write ? buffer : NULL;
        reset();
        assert(transfer_block(5u, rd, wr) == PF_SD_CARD_OK && card_initialized && !selected);
        reset(); command_ok = false;
        assert(transfer_block(5u, rd, wr) == PF_SD_CARD_IO_ERROR);
        assert(!card_initialized && data_calls == 0u && !selected);
        /* A received non-zero R1 proves the card stayed in command mode. */
        reset(); command_response = 0x04u;
        assert(transfer_block(5u, rd, wr) == PF_SD_CARD_IO_ERROR);
        assert(card_initialized && data_calls == 0u && !selected);
    }
    /* Failed read data phase (token timeout, short payload, CRC) is unproven. */
    reset(); data_ok = false;
    assert(transfer_block(5u, buffer, NULL) == PF_SD_CARD_IO_ERROR);
    assert(!card_initialized && data_calls == 1u && !selected);
    reset(); ready = false;
    assert(transfer_block(5u, buffer, NULL) == PF_SD_CARD_IO_ERROR);
    assert(!card_initialized && data_calls == 0u && !selected);
    /* Recovery is reinitialization; failed reads are never retried here. */
    reset(); present = false;
    assert(transfer_block(5u, buffer, NULL) == PF_SD_CARD_NOT_READY && !card_initialized);
    return 0;
}
"""


def main():
    source = SOURCE.read_text()
    transfer = function_source(source, "static pf_sd_card_result_t transfer_blocks(")
    command = function_source(source, "static bool send_command_selected(")
    command = command.replace("send_command_selected", "real_send_command_selected")
    code = HARNESS.replace("@TRANSFER@", transfer).replace("@COMMAND@", command)
    with tempfile.TemporaryDirectory() as scratch:
        src = Path(scratch) / "check.c"
        exe = Path(scratch) / "check"
        src.write_text(code)
        include = SOURCE.parents[1]
        subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                        "-I", str(include), str(src), str(include / "sd_card_protocol.c"),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        block = function_source(source, "static pf_sd_card_result_t transfer_block(")
        src.write_text(SINGLE_HARNESS.replace("@BLOCK@", block))
        subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                        "-I", str(include), "-I", str(include / "spi"), str(src),
                        str(include / "sd_card_protocol.c"),
                        str(include / "spi/sd_spi_protocol.c"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("SPI single- and multi-sector transaction checks passed")


if __name__ == "__main__":
    main()