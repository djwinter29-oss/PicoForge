#!/usr/bin/env python3
"""Exercise production CMD24/CMD25 TX control flow with independent DMA checks."""
import os
from pathlib import Path
import subprocess
import tempfile
from importlib.machinery import SourceFileLoader

ROOT = Path(__file__).resolve().parents[2]
extract = SourceFileLoader('sdio_init_check', str(ROOT / 'tools/validation/check-sdio-init.py')).load_module().function_source
HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "sdio_pio_protocol.h"
#include "sd_card_protocol.h"
#define PF_SDIO_PIO_TRANSFER_WORDS 130u
#define SDIO_DATA_WORD_COUNT 130u
#define SDIO_DATA_NIBBLE_COUNT 1042u
#define DMA_SIZE_32 2u
typedef struct { uint32_t txf[4]; } pio_t;
typedef struct {
    pio_t *pio; unsigned dma_channel, command_sm, data_sm, data_tx_program_offset;
    bool mode_restore_failed, write_stream_open, read_stream_unresolved;
    uint32_t write_stream_next_sector;
} pf_sdio_pio_host_t;
typedef struct { unsigned size, dreq; bool swap, read_inc, write_inc; } dma_channel_config;
static unsigned configurations, defaults, commands, starts, aborts, stops;
static bool timeout, rejected, restore_ok = true, load_ok = true, command_ok = true;
static uint32_t now;
static dma_channel_config dma_channel_get_default_config(unsigned channel)
{ assert(channel == 3); ++defaults; return (dma_channel_config){0}; }
static void channel_config_set_transfer_data_size(dma_channel_config *c, unsigned n) { c->size = n; }
static void channel_config_set_dreq(dma_channel_config *c, unsigned n) { c->dreq = n; }
static void channel_config_set_bswap(dma_channel_config *c, bool n) { c->swap = n; }
static void channel_config_set_read_increment(dma_channel_config *c, bool n) { c->read_inc = n; }
static void channel_config_set_write_increment(dma_channel_config *c, bool n) { c->write_inc = n; }
static unsigned pio_get_dreq(pio_t *p, unsigned sm, bool tx) { assert(p && sm == 1 && tx); return 7; }
static void dma_channel_configure(unsigned channel, const dma_channel_config *c,
                                  void *dest, const void *src, unsigned n, bool start)
{
    assert(channel == 3 && dest && src && ((uintptr_t)src & 3) == 0);
    assert(n == 130 && !start && c->size == DMA_SIZE_32 && c->swap);
    assert(c->read_inc && !c->write_inc && c->dreq == 7);
    assert(pf_sdio_pio_verify_data_block(src)); ++configurations;
}
static void dma_channel_start(unsigned c) { assert(c == 3); ++starts; }
static bool dma_channel_is_busy(unsigned c) { assert(c == 3); return timeout; }
static void dma_channel_abort(unsigned c) { assert(c == 3); ++aborts; }
static void dma_channel_wait_for_finish_blocking(unsigned c) { assert(c == 3 && !timeout); }
static void pio_sm_put_blocking(pio_t *p, unsigned sm, uint32_t n) { assert(p && sm == 1 && n == 1041); }
static void pio_interrupt_clear(pio_t *p, unsigned n) { assert(p && n == 2); }
static bool pio_interrupt_get(pio_t *p, unsigned n) { assert(p && n == 2); return !timeout; }
static void pio_sm_set_enabled(pio_t *p, unsigned sm, bool on) { assert(p && ((sm == 0 && on) || (sm == 1 && !on))); }
static unsigned pio_sm_get_rx_fifo_level(pio_t *p, unsigned sm) { assert(p && sm == 1); return 1; }
static uint32_t pio_sm_get(pio_t *p, unsigned sm) { assert(p && sm == 1); return rejected ? 0x0b : 0x05; }
static void pio_sm_clear_fifos(pio_t *p, unsigned sm) { assert(p && sm == 1); }
static void pio_sm_restart(pio_t *p, unsigned sm) { assert(p && sm == 1); }
static void pio_sm_exec(pio_t *p, unsigned sm, unsigned op) { assert(p && sm == 1 && op == 4); }
static unsigned pio_encode_jmp(unsigned n) { return n; }
static uint32_t time_us_32(void) { now += 500001; return now; }
static void wait_data_idle(pf_sdio_pio_host_t *h, unsigned n) { assert(h && n == 2); }
static void start_tx_data(pf_sdio_pio_host_t *h) { assert(h); }
static bool load_tx_programs(pf_sdio_pio_host_t *h) { assert(h); return load_ok; }
static bool restore_command_mode(pf_sdio_pio_host_t *h) { h->mode_restore_failed = !restore_ok; return restore_ok; }
static bool stop_multiblock_with_clock_fallback(pf_sdio_pio_host_t *h) { assert(h); ++stops; return !timeout; }
static bool pf_sdio_pio_host_command_bytes(pf_sdio_pio_host_t *h, uint8_t cmd,
    uint32_t arg, unsigned bits, uint8_t *r, unsigned size)
{ assert(h && (cmd == 24 || cmd == 25) && arg == 4 && bits == 48 && size == 6); ++commands; memset(r, 0, size); r[0] = cmd; r[5] = (pf_sd_card_crc7(r, 5) << 1) | 1; return command_ok; }
@FUNCTIONS@
int main(void)
{
    pio_t pio = {0};
    pf_sdio_pio_host_t h = { .pio = &pio, .dma_channel = 3, .data_sm = 1, .data_tx_program_offset = 4 };
    uint8_t blocks[1536]; for (unsigned i = 0; i < sizeof(blocks); ++i) blocks[i] = i * 73u;
    assert(pf_sdio_pio_host_write_sector(&h, 4, true, blocks));
    assert(configurations == 1 && starts == 1 && commands == 1);
    rejected = true;
    assert(!pf_sdio_pio_host_write_sector(&h, 4, true, blocks));
    rejected = false; timeout = true;
    assert(!pf_sdio_pio_host_write_sector(&h, 4, true, blocks));
    assert(aborts == 1 && !h.mode_restore_failed);
    timeout = false;
    assert(pf_sdio_pio_host_write_blocks(&h, 4, true, blocks, 3));
    assert(h.write_stream_open && h.write_stream_next_sector == 7);
    unsigned old_commands = commands;
    assert(pf_sdio_pio_host_write_blocks(&h, 7, true, blocks, 1));
    assert(commands == old_commands && h.write_stream_next_sector == 8);
    assert(pf_sdio_pio_host_recover(&h) && !h.write_stream_open);
    timeout = true;
    assert(!pf_sdio_pio_host_write_blocks(&h, 4, true, blocks, 3));
    assert(h.write_stream_open); /* failed stop retains uncertainty */
    timeout = false;
    assert(pf_sdio_pio_host_recover(&h));
    unsigned old_configurations = configurations;
    load_ok = false;
    assert(!pf_sdio_pio_host_write_sector(&h, 4, true, blocks));
    assert(configurations == old_configurations);
    load_ok = true; restore_ok = false;
    assert(!pf_sdio_pio_host_write_sector(&h, 4, true, blocks) && h.mode_restore_failed);
    assert(stops >= 2);
}
'''

def main():
    source = (ROOT / 'firmware/src/drivers/sd_card/sdio_pio/sdio_pio_host.c').read_text()
    names = ['bool pf_sdio_pio_host_write_stream_continues(', 'bool pf_sdio_pio_host_recover(',
             'bool pf_sdio_pio_host_write_sector(', 'bool pf_sdio_pio_host_write_blocks(']
    if 'static dma_channel_config tx_dma_config(' in source:
        names.insert(0, 'static dma_channel_config tx_dma_config(')
    functions = '\n'.join(extract(source, name) for name in names)
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp); (d / 'test.c').write_text(HARNESS.replace('@FUNCTIONS@', functions))
        driver = ROOT / 'firmware/src/drivers/sd_card'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(driver / 'sdio_pio'), '-I' + str(driver), str(d / 'test.c'),
                        str(driver / 'sdio_pio/sdio_pio_protocol.c'), str(driver / 'sd_card_protocol.c'),
                        '-o', str(d / 'test')], check=True)
        subprocess.run([str(d / 'test')], check=True)
    print('Production TX: DMA bounds/CRC, CMD24 timeout, CMD25 continuation/recovery passed')

if __name__ == '__main__':
    main()
