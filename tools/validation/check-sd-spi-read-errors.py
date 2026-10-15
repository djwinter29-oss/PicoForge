#!/usr/bin/env python3
"""Fault-inject actual SPI block reception; no SDK or device access."""
import os
from pathlib import Path
import subprocess
import tempfile
import runpy
ROOT = Path(__file__).resolve().parents[2]
function_source = runpy.run_path(str(Path(__file__).with_name("check-sd-spi-transfer.py")))["function_source"]

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sd_card_protocol.h"
#include "sd_spi_protocol.h"
#define pf_sd_card_profile_begin test_profile_begin
#define pf_sd_card_profile_end test_profile_end
#define pf_sd_card_profile_event test_profile_event
#define pf_sd_card_idle test_idle
#define SD_SPI_RESPONSE_TIMEOUT_US 100000u
static struct { void *spi; } active_config;
static bool card_initialized;
static unsigned exchanges, fail_exchange, reads, crc_calls, events, data_ends, crc_ends;
static uint32_t now, data_elapsed;
static bool pending, invalid, short_payload;
static uint16_t expected_crc;
static uint8_t payload[512];
static uint32_t time_us_32(void) { return now; }
uint32_t pf_sd_card_profile_begin(void) { return now; }
void pf_sd_card_profile_end(pf_sd_card_profile_phase_t phase, uint32_t start)
{
    if (phase == PF_SD_CARD_PROFILE_DATA) { ++data_ends; data_elapsed = now - start; }
    else { assert(phase == PF_SD_CARD_PROFILE_CRC); ++crc_ends; }
}
void pf_sd_card_profile_event(pf_sd_card_profile_event_t event)
{ assert(event == PF_SD_CARD_PROFILE_TIMEOUT); ++events; }
void pf_sd_card_idle(void) { now += 25000u; }
static int spi_write_read_blocking(void *spi, const uint8_t *tx, uint8_t *rx, size_t n)
{
    (void)spi; assert(*tx == 0xffu && n == 1u);
    unsigned index = exchanges++;
    now += 10u;
    if (index == fail_exchange) return 0; /* conditional SDK short transfer */
    if (pending) *rx = 0xffu;
    else if (index == 0u) *rx = invalid ? 0x0bu : 0xfeu;
    else *rx = index == 1u ? (uint8_t)(expected_crc >> 8) : (uint8_t)expected_crc;
    return 1;
}
static int spi_read_blocking(void *spi, uint8_t tx, uint8_t *data, size_t size)
{ (void)spi; (void)tx; ++reads; memcpy(data, payload, size); now += 100u; return short_payload ? (int)size - 1 : (int)size; }
static uint16_t checked_crc(const uint8_t *data, size_t size)
{ ++crc_calls; return pf_sd_card_crc16(data, size); }
@FUNCTIONS@
static void reset(void)
{
    card_initialized = true;
    exchanges = reads = crc_calls = events = data_ends = crc_ends = now = 0;
    fail_exchange = UINT32_MAX;
    pending = invalid = short_payload = false;
    expected_crc = 0;
    memset(payload, 0, sizeof payload);
}
int main(void)
{
    uint8_t data[512];
    reset(); assert(read_data_block(data, sizeof data));
    assert(exchanges == 3 && data_ends == 1 && crc_ends == 1 && data_elapsed == 130);
    for (unsigned i = 0; i < 3; ++i) {
        reset(); fail_exchange = i;
        assert(!read_data_block(data, sizeof data) && !card_initialized);
        assert(data_ends == 1 && crc_calls == 0 && events == 0);
        assert(exchanges == i + 1 && reads == (i != 0));
    }
    reset(); pending = true;
    assert(!read_data_block(data, sizeof data));
    assert(events == 1 && data_ends == 1 && data_elapsed >= SD_SPI_RESPONSE_TIMEOUT_US);
    assert(reads == 0 && crc_calls == 0);
    reset(); invalid = true;
    assert(!read_data_block(data, sizeof data) && events == 0 && data_ends == 1);
    reset(); short_payload = true;
    assert(!read_data_block(data, sizeof data) && exchanges == 1 && crc_calls == 0 && data_ends == 1);
    /* CRC=ffff makes the old default-byte fallback capable of false success. */
    reset();
    bool found = false;
    for (unsigned value = 0; value <= UINT16_MAX; ++value) {
        payload[510] = value >> 8; payload[511] = value;
        if (pf_sd_card_crc16(payload, sizeof payload) == 0xffffu) { found = true; break; }
    }
    assert(found);
    expected_crc = 0xffffu;
    fail_exchange = 2;
    assert(!read_data_block(data, sizeof data) && !card_initialized && crc_calls == 0);
    puts("SPI token/CRC-tail short-transfer, timeout/profile and payload faults passed");
}
'''

source = (ROOT / 'firmware/src/drivers/sd_card/spi/sd_spi.c').read_text()
functions = '\n'.join(function_source(source, sig) for sig in (
    'static bool exchange_byte_checked(', 'static bool read_data_block('))
functions = functions.replace('pf_sd_card_crc16(data, size)', 'checked_crc(data, size)')
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'test.c'
    src.write_text(HARNESS.replace('@FUNCTIONS@', functions))
    driver = ROOT / 'firmware/src/drivers/sd_card'
    exe = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    '-I'+str(driver), '-I'+str(driver/'spi'), str(src),
                    str(driver/'sd_card_protocol.c'), str(driver/'spi/sd_spi_protocol.c'),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
