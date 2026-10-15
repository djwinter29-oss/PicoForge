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
/* Match the public protocol mask used by the production extraction below. */
#define PF_SDIO_PIO_R1_ERROR_MASK 0xfdffe008u
#define SDIO_DATA_READ_IRQ 1u
#define SDIO_DATA_NIBBLE_COUNT 1040u
#define SDIO_DATA_WORD_COUNT 130u
#define SDIO_READ_BATCH_BLOCKS 64u
typedef struct { uint32_t rxf[4], txf[4]; } fake_pio_t;
typedef struct {
    fake_pio_t *pio;
    unsigned data_sm, dma_channel, data_rx_program_offset;
    bool data_rx_program_loaded, data_rx_multiblock_supported, read_stream_unresolved, write_stream_open, mode_restore_failed;
} pf_sdio_pio_host_t;
static uint32_t read_wire_buffer[130u * 64u];
static bool response_ok, crc_ok, stop_ok, timeout, command_ok;
static uint32_t response_status;
static unsigned single_calls, stops, commands, cleanup_calls;
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
    assert(destination != NULL);
    if (block_count > 1u) assert(destination == read_wire_buffer);
    rx_transfer_words = transfer_words;
    rx_nibble_count = nibble_count;
    rx_block_count = block_count;
}
static void cleanup_data_rx(pf_sdio_pio_host_t *host) { (void)host; ++cleanup_calls; }
static void wait_data_idle(pf_sdio_pio_host_t *host, uint32_t completion_flag)
{ (void)host; (void)completion_flag; }
static bool pf_sdio_pio_host_command_bytes(pf_sdio_pio_host_t *host, uint8_t command,
    uint32_t argument, unsigned bits, uint8_t *response, unsigned size)
{ assert(!host->read_stream_unresolved && command == 18u); ++commands;
  (void)argument; (void)bits; (void)response; (void)size; return command_ok; }
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
static bool restore_command_mode(pf_sdio_pio_host_t *host) { (void)host; return true; }
@CLOSE@
@READER@
int main(void)
{
    fake_pio_t pio = {0};
    pf_sdio_pio_host_t host = { .pio = &pio, .data_rx_program_loaded = true,
                               .data_rx_multiblock_supported = true };
    uint8_t blocks[1024];
    unsigned previous_stops;
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
    command_ok = false;
    previous_stops = stops;
    assert(!pf_sdio_pio_host_read_blocks(&host, 0u, true, blocks, 2u));
    assert(host.data_rx_multiblock_supported && single_calls == 0u);
    assert(stops == previous_stops + 1u && !host.read_stream_unresolved);
    command_ok = true;
    response_ok = false;
    previous_stops = stops;
    assert(!pf_sdio_pio_host_read_blocks(&host, 0u, true, blocks, 2u));
    assert(host.data_rx_multiblock_supported && single_calls == 0u);
    assert(stops == previous_stops + 1u && !host.read_stream_unresolved);
    response_ok = true;
    stop_ok = false;
    assert(!pf_sdio_pio_host_read_blocks(&host, 0u, true, blocks, 2u));
    assert(host.data_rx_multiblock_supported && single_calls == 0u);
    assert(host.read_stream_unresolved);
    previous_stops = stops;
    assert(!pf_sdio_pio_host_read_blocks(&host, 0u, true, blocks, 2u));
    assert(stops == previous_stops); /* cannot issue more I/O before recovery */
    assert(!pf_sdio_pio_host_recover(&host));
    assert(host.read_stream_unresolved);
    stop_ok = true;
    assert(pf_sdio_pio_host_recover(&host));
    assert(!host.read_stream_unresolved);
    assert(pf_sdio_pio_host_read_blocks(&host, 0u, true, NULL, 2u));
    for (unsigned failure = 0u; failure < 2u; ++failure) {
        command_ok = failure != 0u;
        response_ok = failure == 0u;
        stop_ok = false;
        assert(!pf_sdio_pio_host_read_blocks(&host, 0u, true, blocks, 2u));
        assert(host.read_stream_unresolved);
        unsigned previous_commands = commands;
        previous_stops = stops;
        assert(!pf_sdio_pio_host_read_blocks(&host, 0u, true, blocks, 1u));
        assert(commands == previous_commands && stops == previous_stops);
        assert(!pf_sdio_pio_host_recover(&host));
        assert(host.read_stream_unresolved);
        stop_ok = true;
        assert(pf_sdio_pio_host_recover(&host));
        assert(!host.read_stream_unresolved);
        command_ok = response_ok = true;
        assert(pf_sdio_pio_host_read_blocks(&host, 0u, true, blocks, 2u));
    }
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
        close_signature = "bool pf_sdio_pio_host_recover"
        close_start = source.index(close_signature)
        close = (source[close_start:source.index("{", close_start)] +
                 function_body(source, close_signature) + "}")
        src.write_text(READER_HARNESS.replace("@READER@", reader).replace("@CLOSE@", close))
        include = HOST.parents[1]
        build = subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                                "-I", str(include), str(src),
                                str(include / "sd_card_protocol.c"), "-o", str(exe)],
                               capture_output=True, text=True)
        if build.returncode:
            return ["reader harness compile failed: " + build.stderr]
        result = subprocess.run([str(exe)])
        return [] if result.returncode == 0 else ["reader recovery behavior failed"]


# Run the production command gate, CMD12 validation/busy loop, clock fallback,
# and mode restoration together with the reader. Only hardware/program loading,
# response decoding and payload decoding are injected at their boundaries.
RECOVERY_HARDWARE = r"""
#define SDIO_RESPONSE_IRQ 0u
#define pio_sm_set_enabled(...) ((void)0)
#define pio_sm_clear_fifos(...) ((void)0)
#define pio_interrupt_clear(...) ((void)0)
#define pio_sm_restart(...) ((void)0)
#define pio_sm_exec(...) ((void)0)
#define pio_sm_set_clkdiv_int_frac(...) ((void)0)
#define pio_remove_program(...) ((void)0)
#define PF_SDIO_PIO_TRANSFER_WORDS 130u
#define dma_channel_wait_for_finish_blocking(channel) ((void)0)
typedef unsigned dma_channel_config;
#define DMA_SIZE_32 0u
#define dma_channel_get_default_config(channel) ((void)(channel), 0u)
#define channel_config_set_transfer_data_size(config, value) ((void)(config))
#define channel_config_set_bswap(config, value) ((void)(config))
#define channel_config_set_read_increment(config, value) ((void)(config))
#define channel_config_set_write_increment(config, value) ((void)(config))
#define channel_config_set_dreq(config, value) ((void)(config))
#define dma_channel_configure(channel, config, dest, source, count, start) ((void)(config))
#define dma_channel_start(channel) ((void)0)
#define dma_channel_abort(channel) ((void)0)
#define pio_sm_get_rx_fifo_level(pio, sm) 1u
#define pio_sm_get(pio, sm) 0u
static bool wait_data_rx(pf_sdio_pio_host_t *host, uint32_t deadline)
{ (void)host; (void)deadline; return !timeout; }
static bool pf_sdio_pio_decode_data_block_sized(const uint8_t *image, uint8_t *data, size_t size)
{ (void)image; (void)data; (void)size; return crc_ok; }
static bool pf_sdio_pio_verify_data_block(const uint8_t *image) { (void)image; return crc_ok; }
static bool accepted_ok = true;
static bool pf_sdio_pio_write_response_accepted(unsigned token) { (void)token; return accepted_ok; }
static bool pf_sdio_pio_encode_data_block(const uint8_t *block, uint8_t *image)
{ memcpy(image, block, 512u); return true; }
static bool load_tx_programs(pf_sdio_pio_host_t *host)
{ host->command_program_loaded = host->data_rx_program_loaded = false; return true; }
static void start_tx_data(pf_sdio_pio_host_t *host) { (void)host; }
static unsigned fifo_puts, last_command, stop_attempts;
static bool load_ok = true, stop_response_ok, busy_low, cmd18_wire_ok = true;
static void sleep_us(uint32_t us) { (void)us; }
static unsigned gpio_get(unsigned gpio) { (void)gpio; return !busy_low; }
static bool load_command_program(pf_sdio_pio_host_t *host)
{ host->command_program_loaded = load_ok; return load_ok; }
static bool load_data_rx_program(pf_sdio_pio_host_t *host)
{ host->data_rx_program_loaded = load_ok; return load_ok; }
static void pio_sm_put_blocking(fake_pio_t *pio, unsigned sm, uint32_t word)
{
    (void)pio;
    if (sm != 0u) return;
    if (fifo_puts++ % 3u == 0u) last_command = (word >> 18u) & 63u;
}
static bool wait_for_command_complete(pf_sdio_pio_host_t *host,
                                      uint32_t *words, size_t count)
{
    (void)host; (void)words; (void)count;
    ++commands;
    if (last_command == 12u) ++stop_attempts;
    return last_command != 18u || cmd18_wire_ok;
}
static bool pf_sdio_pio_response_words_to_bytes(const uint32_t *words,
    uint16_t bits, uint8_t *bytes, size_t capacity)
{ (void)words; (void)bits; (void)bytes; (void)capacity; return true; }
static bool pf_sdio_pio_build_command(uint8_t command, uint32_t arg, uint8_t frame[6])
{ (void)arg; memset(frame, 0, 6u); frame[0] = 0x40u | command; return true; }
static bool pf_sdio_pio_parse_response(uint8_t command, const uint8_t *response,
                                      uint32_t *argument, bool crc)
{ (void)response; assert(crc); *argument = response_status;
  return command == 12u ? stop_response_ok : response_ok; }
"""

RECOVERY_MAIN = r"""
int main(void)
{
    fake_pio_t pio = {0};
    pf_sdio_pio_host_t host = { .pio = &pio, .data_rx_program_loaded = true,
        .command_program_loaded = true, .data_rx_multiblock_supported = true,
        .data_sm = 1u, .system_clock_hz = 200000000u, .current_clock_hz = 50000000u };
    uint8_t blocks[1024];
    uint8_t payload[512] = {0};
    response_ok = true;
    /* A real single-sector write accepts data but fails program restoration. */
    load_ok = false;
    assert(!pf_sdio_pio_host_write_sector(&host, 0u, true, payload));
    assert(host.mode_restore_failed && !host.write_stream_open && !host.read_stream_unresolved);
    assert(!pf_sdio_pio_host_recover(&host));
    unsigned failed_commands = commands;
    assert(!pf_sdio_pio_host_command(&host, 0u, 0u, 0u, NULL, 0u));
    assert(commands == failed_commands);
    load_ok = true;
    assert(card_reinit(&host) == PF_SD_CARD_OK);
    assert(!host.mode_restore_failed && !host.read_stream_unresolved);
    /* An ordinary card rejection restores successfully and remains distinct. */
    accepted_ok = false;
    assert(!pf_sdio_pio_host_write_sector(&host, 1u, true, payload));
    assert(!host.mode_restore_failed);
    assert(pf_sdio_pio_host_recover(&host));
    accepted_ok = true;
    init_calls = 0u;
    response_ok = false;
    host.current_clock_hz = 50000000u;
    crc_ok = true;
    response_ok = true;
    unsigned old_cleanups = cleanup_calls;
    assert(pf_sdio_pio_host_read_sector(&host, 0u, true, payload));
    assert(pf_sdio_pio_host_read_sector(&host, 0u, true, NULL));
    assert(rx_transfer_words == 130u && rx_nibble_count == 1040u && rx_block_count == 1u);
    crc_ok = false;
    assert(!pf_sdio_pio_host_read_sector(&host, 0u, true, payload));
    assert(!pf_sdio_pio_host_read_sector(&host, 0u, true, NULL));
    crc_ok = true;
    response_ok = false;
    assert(!pf_sdio_pio_host_read_sector(&host, 0u, true, payload));
    assert(cleanup_calls == old_cleanups + 5u);
    assert(!read_data_block(&host, 6u, 0u, NULL, 64u));
    assert(cleanup_calls == old_cleanups + 5u);
    /* CMD18 response lost; all six CMD12 response validations fail. */
    assert(!pf_sdio_pio_host_read_blocks(&host, 0u, true, blocks, 2u));
    assert(stop_attempts == 6u && host.read_stream_unresolved);
    unsigned previous = commands;
    assert(!pf_sdio_pio_host_command(&host, 0u, 0u, 0u, NULL, 0u));
    unsigned previous_init = init_calls;
    assert(card_reinit(&host) == PF_SD_CARD_INIT_FAILED);
    assert(init_calls == previous_init && host.read_stream_unresolved);
    previous = commands;
    assert(!pf_sdio_pio_host_command(&host, 13u, 0u, 0u, NULL, 0u));
    assert(!pf_sdio_pio_host_read_blocks(&host, 0u, true, blocks, 1u));
    assert(commands == previous);
    /* Even a valid CMD12 response cannot resolve DAT0 busy timeout. */
    stop_response_ok = true;
    busy_low = true;
    host.current_clock_hz = 50000000u;
    assert(!pf_sdio_pio_host_recover(&host));
    assert(stop_attempts == 13u && host.read_stream_unresolved);
    /* Failure to reload either command or RX program preserves both streams. */
    busy_low = false;
    host.write_stream_open = true;
    load_ok = false;
    host.command_program_loaded = false;
    assert(!pf_sdio_pio_host_recover(&host));
    assert(host.read_stream_unresolved && host.write_stream_open && stop_attempts == 13u);
    host.command_program_loaded = true;
    host.data_rx_program_loaded = false;
    assert(!pf_sdio_pio_host_recover(&host));
    assert(host.read_stream_unresolved && host.write_stream_open && stop_attempts == 13u);
    load_ok = true;
    assert(card_reinit(&host) == PF_SD_CARD_OK);
    assert(card_initialized && init_calls == previous_init + 1u);
    assert(!host.read_stream_unresolved && !host.write_stream_open && stop_attempts == 14u);
    assert(pf_sdio_pio_host_command(&host, 0u, 0u, 0u, NULL, 0u));
    response_ok = true;
    assert(pf_sdio_pio_host_read_blocks(&host, 0u, true, blocks, 2u));
    /* Lost command completion also follows real CMD18 cleanup and stop retries. */
    cmd18_wire_ok = stop_response_ok = false;
    host.current_clock_hz = 50000000u;
    unsigned previous_stops = stop_attempts;
    assert(!pf_sdio_pio_host_read_blocks(&host, 0u, true, blocks, 2u));
    assert(host.read_stream_unresolved && stop_attempts == previous_stops + 6u);
    stop_response_ok = true;
    assert(card_reinit(&host) == PF_SD_CARD_OK && !host.read_stream_unresolved);
    puts("Production SDIO command/stop recovery checks passed");
    return 0;
}
"""


def production_recovery_check(source):
    code = READER_HARNESS[:READER_HARNESS.index("int main(void)")]
    for name in ("pf_sdio_pio_host_command_bytes", "pf_sdio_pio_parse_response",
                 "stop_multiblock_with_clock_fallback", "restore_command_mode"):
        start = code.index("static bool " + name + "(")
        body = function_body(code, "static bool " + name)
        end = code.index("{", start) + len(body) + 1
        code = code[:start] + code[end:]
    code = code.replace('#include <stdint.h>', '#include <stdint.h>\n#include <stddef.h>\n#include <stdio.h>\n#include <string.h>')
    code = code.replace('unsigned data_sm, dma_channel, data_rx_program_offset;',
        'unsigned data_sm, dma_channel, data_rx_program_offset, command_sm, data0_gpio;\n'
        'uint32_t system_clock_hz, current_clock_hz, clock_div_ticks;\n'
        'bool command_program_loaded, data_tx_program_loaded, clock_program_loaded;')
    code = code.replace('response_ok, crc_ok, stop_ok, timeout, command_ok', 'response_ok, crc_ok, timeout')
    code = code.replace('single_calls, stops, commands', 'single_calls, commands')
    code = code.replace('@CLOSE@\n@READER@', RECOVERY_HARDWARE)
    for signature in ('bool pf_sdio_pio_host_set_clock', 'bool pf_sdio_pio_host_command',
                      'bool pf_sdio_pio_host_command_bytes', 'static bool restore_command_mode',
                      'static bool stop_multiblock_transfer', 'static bool stop_multiblock_with_clock_fallback',
                      'bool pf_sdio_pio_host_recover', 'static bool read_data_block',
                      'bool pf_sdio_pio_host_read_sector', 'static dma_channel_config tx_dma_config', 'bool pf_sdio_pio_host_write_sector', 'bool pf_sdio_pio_host_read_blocks'):
        start = source.index(signature + '(')
        code += source[start:source.index('{', start)] + function_body(source, signature) + '}\n'
    # Extract the card-layer init tail: actual stop-before-identification ordering.
    # Identification itself is a stub that issues real host CMD0; no enumeration claim.
    card_source = (HOST.parent / 'sdio_pio.c').read_text()
    init_body = function_body(card_source, 'pf_sd_card_result_t pf_sdio_pio_init')
    init_tail = init_body[init_body.index('    if (!card_detect_present())'):]
    init_tail = init_tail.replace('&sdio_pio_host', 'host')
    code += r"""
static bool card_initialized;
static unsigned init_calls, card_init_stage;
#define PF_SDIO_PIO_INIT_STAGE_TRANSFER_CLOCK 1u
static struct { uint32_t clock_hz; } active_config = { 25000000u };
static const uint32_t transfer_clock_steps_hz[] = {25000000u, 10000000u};
static pf_sdio_pio_host_t *initializing_host;
static bool card_detect_present(void) { return true; }
static uint32_t default_clock_hz(void) { return 10000000u; }
static bool initialize_card(uint32_t *limit)
{
    (void)limit;
    ++init_calls;
    assert(!initializing_host->read_stream_unresolved);
    assert(initializing_host->current_clock_hz == 400000u);
    return pf_sdio_pio_host_command(initializing_host, 0u, 0u, 0u, NULL, 0u);
}
static pf_sd_card_result_t card_reinit(pf_sdio_pio_host_t *host)
{
    initializing_host = host;
""" + init_tail + '}\n'
    code += RECOVERY_MAIN
    with tempfile.TemporaryDirectory() as scratch:
        src, exe = Path(scratch) / 'recovery.c', Path(scratch) / 'recovery'
        src.write_text(code)
        include = HOST.parents[1]
        build = subprocess.run([os.environ.get('CC', 'cc'), '-Wall', '-Wextra', '-Werror',
                                '-I', str(include), str(src), str(include / 'sd_card_protocol.c'),
                                '-o', str(exe)], capture_output=True, text=True)
        if build.returncode:
            return ['production recovery compile failed: ' + build.stderr]
        result = subprocess.run([str(exe)])
        return [] if result.returncode == 0 else ['production recovery behavior failed']


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
    errors = behavior_check(source) + reader_behavior_check(source) + production_recovery_check(source)

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
