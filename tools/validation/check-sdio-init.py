#!/usr/bin/env python3
"""Exercise SDIO qualification, retry, and transfer routing without a Pico SDK."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "firmware/src/drivers/sd_card/sdio_pio/sdio_pio.c"

VARIANT_HARNESS = r"""
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "sdio_pio_protocol.h"
int main(void)
{
    uint8_t input[515], output[515], wire[523];
    for (unsigned alignment = 0u; alignment < 4u; ++alignment) {
        uint8_t *block = input + alignment;
        uint8_t *decoded = output + alignment;
        uint8_t *image = wire + alignment;
        uint16_t expected[4] = {0}, actual[4];
        for (unsigned byte = 0u; byte < 512u; ++byte) {
            block[byte] = (uint8_t)(byte * 73u + alignment);
            for (unsigned half = 0u; half < 2u; ++half) {
                uint8_t nibble = (uint8_t)(block[byte] >> (half == 0u ? 4u : 0u));
                for (unsigned lane = 0u; lane < 4u; ++lane) {
                    bool feedback = ((expected[lane] >> 15u) ^ (nibble >> lane)) & 1u;
                    expected[lane] = (uint16_t)(expected[lane] << 1u);
                    if (feedback) expected[lane] ^= 0x1021u;
                }
            }
        }
        pf_sdio_pio_crc16_4bit(block, actual);
        assert(memcmp(expected, actual, sizeof(actual)) == 0);
        assert(pf_sdio_pio_encode_data_block(block, image));
        assert(memcmp(block, image, 512u) == 0);
        assert(pf_sdio_pio_verify_data_block(image));
        assert(pf_sdio_pio_decode_data_block(image, decoded));
        assert(memcmp(block, decoded, 512u) == 0);
        image[512] ^= 0x01u;
        assert(!pf_sdio_pio_verify_data_block(image));
        assert(!pf_sdio_pio_decode_data_block(image, decoded));
    }
    return 0;
}
"""

WAIT_HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define PF_CONFIG_SD_CARD_INTERRUPT_WAIT 1
#define PF_SD_CARD_TRANSPORT_SDIO_PIO 0
#define pdFALSE 0
#define pdTRUE 1
#define taskSCHEDULER_RUNNING 1
#define PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY 0
#define PICO_DEFAULT_IRQ_PRIORITY 128
typedef unsigned uint;
typedef int BaseType_t;
typedef void *TaskHandle_t;
typedef struct { bool flag[3], source[3]; } fake_pio_t;
typedef fake_pio_t *PIO;
typedef enum { pis_interrupt0 = 0, pis_interrupt1, pis_interrupt2 } pio_interrupt_source_t;
typedef struct { int transport; struct { struct { PIO pio; } sdio_pio; } bus; } pf_sd_card_config_t;
static void (*irq_handler)(void);
static void (*wait_hook)(PIO, uint32_t);
static PIO configured_pio;
static unsigned scheduler_state, notifications, blocked, notified, critical;
static bool early, late;
static bool pio_interrupt_get(PIO pio, uint32_t flag) { return pio->flag[flag]; }
static void pio_set_irq0_source_enabled(PIO pio, pio_interrupt_source_t source, bool enabled)
{ pio->source[source] = enabled; }
static void critical_enter(void) { ++critical; }
static void critical_exit(void)
{
    assert(critical == 1u);
    --critical;
    if (early) {
        early = false;
        configured_pio->flag[1] = true;
        irq_handler();
    }
}
#define taskENTER_CRITICAL() critical_enter()
#define taskEXIT_CRITICAL() critical_exit()
#define portYIELD_FROM_ISR(awakened) ((void)(awakened))
static unsigned xTaskGetSchedulerState(void) { return scheduler_state; }
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)(uintptr_t)1u; }
static void vTaskNotifyGiveIndexedFromISR(TaskHandle_t task, unsigned index, BaseType_t *awakened)
{
    assert(task == (void *)(uintptr_t)1u && index == 1u && critical == 0u);
    ++notifications;
    ++notified;
    *awakened = pdTRUE;
}
static uint32_t ulTaskNotifyTakeIndexed(unsigned index, BaseType_t clear, unsigned ticks)
{
    assert(index == 1u && clear == pdTRUE && ticks <= 1u);
    if (ticks != 0u) {
        ++blocked;
        if (late) {
            late = false;
            configured_pio->flag[2] = true;
            irq_handler();
        }
    }
    uint32_t result = notifications;
    notifications = 0u;
    return result;
}
static int pio_get_irq_num(PIO pio, uint index)
{ configured_pio = pio; assert(index == 0u); return 7; }
static void irq_add_shared_handler(uint irq, void (*handler)(void), unsigned order)
{ assert(irq == 7u && order == 0u); irq_handler = handler; }
static void irq_set_priority(uint irq, unsigned priority) { assert(irq == 7u && priority == 128u); }
static void irq_set_enabled(uint irq, bool enabled) { assert(irq == 7u && enabled); }
static void pf_sdio_pio_set_wait_hook(void (*hook)(PIO, uint32_t)) { wait_hook = hook; }
@ADAPTER@
int main(void)
{
    fake_pio_t pio = {0}, foreign = {0};
    pf_sd_card_config_t config = { .transport = 1, .bus.sdio_pio.pio = &pio };
    pf_sd_card_wait_init(NULL);
    pf_sd_card_wait_init(&config);
    assert(wait_hook == NULL);
    config.transport = 0;
    pf_sd_card_wait_init(&config);
    assert(wait_hook != NULL && !pio.source[1] && !pio.source[2]);
    wait_hook(&pio, 1u);
    assert(blocked == 0u);
    scheduler_state = taskSCHEDULER_RUNNING;
    wait_hook(&foreign, 1u);
    wait_hook(&pio, 0u);
    assert(blocked == 0u);
    notifications = 9u;
    wait_hook(&pio, 1u);
    assert(blocked == 1u && notifications == 0u && waiting_task == NULL);
    assert(!pio.source[1] && !pio.flag[1]);
    early = true;
    wait_hook(&pio, 1u);
    assert(notified == 1u && pio.flag[1] && !pio.source[1] && waiting_task == NULL);
    unsigned previous = blocked;
    wait_hook(&pio, 1u);
    assert(blocked == previous);
    late = true;
    wait_hook(&pio, 2u);
    assert(notified == 2u && pio.flag[2] && !pio.source[2] && waiting_task == NULL);
    return 0;
}
"""


def check_optional_variants():
    cc = os.environ.get("CC", "cc")
    include = SOURCE.parents[1]
    adapter = ROOT / "firmware/src/drivers/sd_card/freertos/sd_card_wait.c"
    adapter_source = "\n".join(line for line in adapter.read_text().splitlines()
                               if not line.startswith("#include"))
    with tempfile.TemporaryDirectory() as scratch:
        src = Path(scratch) / "variant.c"
        exe = Path(scratch) / "variant"
        src.write_text(VARIANT_HARNESS)
        for fused in (0, 1):
            subprocess.run([cc, "-Wall", "-Wextra", "-Werror",
                            "-DPF_SDIO_PIO_FUSED_COPY=" + str(fused),
                            "-I", str(include), "-I", str(SOURCE.parent), str(src),
                            str(include / "sd_card_protocol.c"),
                            str(SOURCE.parent / "sdio_pio_protocol.c"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
        src.write_text(WAIT_HARNESS.replace("@ADAPTER@", adapter_source))
        subprocess.run([cc, "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("Native copy variants and interrupt wait race checks passed")


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
#include "sd_card_protocol.h"
#define PF_SDIO_PIO_INIT_STAGE_TRANSFER_CLOCK 1
#define PF_SDIO_PIO_INIT_STAGE_NONE 0
static struct {
    uint32_t current_clock_hz;
    bool data_rx_multiblock_supported;
    bool write_stream_open;
    bool mode_restore_failed;
} sdio_pio_host;
typedef struct {
    uint32_t clock_hz;
    bool use_card_detect;
    unsigned card_detect_gpio;
    bool card_detect_active_high;
} pf_sdio_pio_config_t;
static pf_sdio_pio_config_t active_config = { .clock_hz = 50000000u };
static uint32_t card_sector_count = 15564800u;
static uint8_t card_cid[16];
static bool card_block_addressed = true, card_initialized, configuration_set = true;
static unsigned card_init_stage, attempts, resets, reads, sectors, high_speed_requests;
static uint32_t system_hz = 150000000u;
static bool corrupt_fast, fail_reads, lose_batching, fail_identification;
static bool runtime_read;
static bool change_cid, change_capacity;
static uint32_t fail_above_hz;
static bool removed, continue_write, close_ok = true, write_ok = true, fail_write_restore;
static unsigned closes, single_writes, batch_writes;
static unsigned detect_gpio_level = 1u, forgotten_removed_cards;
static uint32_t pf_sdio_pio_block_count(void) { return card_sector_count; }
static void pf_sdio_pio_host_forget_removed_card(void *host)
{
    (void)host;
    ++forgotten_removed_cards;
    sdio_pio_host.write_stream_open = false;
}
static bool pf_sdio_pio_host_write_stream_continues(void *host, uint32_t first)
{
    (void)host; (void)first;
    return continue_write;
}
static bool pf_sdio_pio_host_recover(void *host)
{
    (void)host;
    ++closes;
    if (close_ok) {
        sdio_pio_host.write_stream_open = false;
        sdio_pio_host.mode_restore_failed = false;
    }
    return close_ok;
}
static bool pf_sdio_pio_host_write_sector(void *host, uint32_t first,
                                          bool addressed, const uint8_t *buffer)
{
    (void)host; (void)first;
    assert(addressed && buffer != NULL);
    ++single_writes;
    sdio_pio_host.mode_restore_failed = fail_write_restore;
    return write_ok && !fail_write_restore;
}
static bool pf_sdio_pio_host_write_blocks(void *host, uint32_t first,
                                          bool addressed, const uint8_t *buffer,
                                          uint32_t count)
{
    (void)host; (void)first;
    assert(addressed && buffer != NULL && count > 0u);
    ++batch_writes;
    sdio_pio_host.write_stream_open = write_ok || !close_ok;
    return write_ok;
}
static unsigned gpio_get(unsigned pin) { (void)pin; return detect_gpio_level; }
@IS_INITIALIZED@
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
#define clk_sys 0
static uint32_t clock_get_hz(unsigned clk) { (void)clk; return system_hz; }
static bool pins_are_valid(const pf_sdio_pio_config_t *config) { (void)config; return true; }
@CONFIG_VALID@
@LADDER@
@PROBE@
@READ_RETRY@
@TRANSFER@
@VALIDATE_32@
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
    closes = single_writes = batch_writes = 0u;
    removed = continue_write = false;
    configuration_set = true;
    detect_gpio_level = 1u;
    forgotten_removed_cards = 0u;
    close_ok = write_ok = true;
    fail_write_restore = sdio_pio_host.mode_restore_failed = false;
    high_speed_requests = 0u;
    sdio_pio_host.write_stream_open = false;
    card_initialized = false;
    card_init_stage = 0u;
    corrupt_fast = fail_reads = lose_batching = fail_identification = false;
    runtime_read = false;
    change_cid = change_capacity = false;
    memset(card_cid, 0, sizeof(card_cid));
    active_config.use_card_detect = false;
    active_config.card_detect_active_high = true;
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
    assert(attempts == 8u && reads == 7u && resets == 9u);
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
    for (unsigned i = 0; i < 3u; ++i) {
        const uint32_t low_rates[] = {400000u, 999999u, 1000000u};
        reset();
        active_config.clock_hz = low_rates[i];
        assert(config_is_valid(&active_config));
        assert(initialize() == PF_SD_CARD_OK);
        assert(sdio_pio_host.current_clock_hz == (i == 2u ? 1000000u : 400000u));
    }
    active_config.clock_hz = 399999u;
    assert(!config_is_valid(&active_config));
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
    assert(!card_initialized && reads == 13u);
    card_initialized = false;
    assert(!read_blocks_with_clock_fallback(4098u, buffer, 1u));
    assert(reads == 13u);
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
    detect_gpio_level = 0u;
    assert(!read_blocks_with_clock_fallback(4096u, buffer, 1u));
    assert(!card_initialized && attempts == 0u && reads == 0u);
    reset();
    runtime_read = card_initialized = true;
    sdio_pio_host.current_clock_hz = 25000000u;
    assert(transfer_blocks(0u, NULL, NULL, 0u) == PF_SD_CARD_OK && closes == 0u);
    assert(transfer_blocks(0u, NULL, NULL, 1u) == PF_SD_CARD_INVALID_ARGUMENT);
    assert(transfer_blocks(card_sector_count, buffer, NULL, 1u) == PF_SD_CARD_INVALID_ARGUMENT);
    assert(closes == 0u && reads == 0u);
    removed = true;
    active_config.use_card_detect = true;
    active_config.card_detect_active_high = true;
    detect_gpio_level = 0u;
    assert(transfer_blocks(0u, NULL, buffer, 2u) == PF_SD_CARD_NOT_READY);
    assert(closes == 0u && batch_writes == 0u);
    removed = false;
    detect_gpio_level = 1u;
    card_initialized = true;
    assert(!sdio_pio_host.write_stream_open);
    assert(transfer_blocks(0u, buffer, NULL, 1u) == PF_SD_CARD_OK);
    assert(reads == 1u && closes == 1u && single_writes == 0u);
    assert(transfer_blocks(0u, NULL, buffer, 1u) == PF_SD_CARD_OK);
    assert(single_writes == 1u && closes == 2u);
    assert(transfer_blocks(0u, NULL, buffer, 2u) == PF_SD_CARD_OK);
    assert(batch_writes == 1u && closes == 3u);
    continue_write = true;
    assert(transfer_blocks(2u, NULL, buffer, 1u) == PF_SD_CARD_OK);
    assert(batch_writes == 2u && closes == 3u && single_writes == 1u);
    write_ok = false;
    close_ok = true;
    assert(transfer_blocks(3u, NULL, buffer, 1u) == PF_SD_CARD_IO_ERROR);
    assert(card_initialized && !sdio_pio_host.write_stream_open);
    continue_write = false;
    write_ok = true;
    assert(transfer_blocks(3u, NULL, buffer, 1u) == PF_SD_CARD_OK);
    assert(single_writes == 2u && card_initialized);

    continue_write = true;
    write_ok = false;
    close_ok = false;
    assert(transfer_blocks(4u, NULL, buffer, 1u) == PF_SD_CARD_IO_ERROR);
    continue_write = false;
    assert(!card_initialized);
    unsigned previous_batch_writes = batch_writes;
    assert(transfer_blocks(4u, NULL, buffer, 1u) == PF_SD_CARD_NOT_READY);
    assert(batch_writes == previous_batch_writes);
    card_initialized = true;
    sdio_pio_host.write_stream_open = true;
    assert(transfer_blocks(0u, buffer, NULL, 1u) == PF_SD_CARD_IO_ERROR);
    assert(!card_initialized);
    assert(reads == 1u && single_writes == 2u && batch_writes == 4u);
    reset();
    card_initialized = true;
    fail_write_restore = true;
    assert(transfer_blocks(1u, NULL, buffer, 1u) == PF_SD_CARD_IO_ERROR);
    assert(!card_initialized && sdio_pio_host.mode_restore_failed && single_writes == 1u);
    assert(!pf_sdio_pio_is_initialized());
    assert(pf_sdio_pio_sync() == PF_SD_CARD_NOT_READY);
    assert(transfer_blocks(1u, NULL, buffer, 1u) == PF_SD_CARD_NOT_READY);
    close_ok = false;
    assert(initialize() == PF_SD_CARD_INIT_FAILED);
    assert(single_writes == 1u && sdio_pio_host.mode_restore_failed);
    close_ok = true;
    fail_write_restore = false;
    assert(initialize() == PF_SD_CARD_OK);
    assert(pf_sdio_pio_sync() == PF_SD_CARD_OK && single_writes == 1u);
    assert(transfer_blocks(2u, NULL, buffer, 1u) == PF_SD_CARD_OK && single_writes == 2u);
    reset();
    card_initialized = true;
    active_config.use_card_detect = true;
    active_config.card_detect_active_high = true;
    sdio_pio_host.write_stream_open = true;
    detect_gpio_level = 0u;
    pf_sd_card_info_t info;
    pf_sdio_pio_get_info(&info);
    assert(info.presence_known && !info.present && !info.initialized);
    assert(!pf_sdio_pio_is_initialized());
    assert(!card_initialized && sdio_pio_host.write_stream_open &&
        forgotten_removed_cards == 0u);
    unsigned previous_closes = closes;
    assert(pf_sdio_pio_sync() == PF_SD_CARD_NOT_READY);
    assert(closes == previous_closes);
    detect_gpio_level = 1u; /* bounce back: reinit must still close CMD25 */
    assert(!pf_sdio_pio_is_initialized());
    close_ok = false;
    assert(initialize() == PF_SD_CARD_INIT_FAILED);
    assert(!card_initialized && sdio_pio_host.write_stream_open);
    assert(attempts == 0u && resets == 0u);
    close_ok = true;
    assert(initialize() == PF_SD_CARD_OK);
    assert(card_initialized && !sdio_pio_host.write_stream_open);
    assert(attempts == 1u && resets == 1u);
    sdio_pio_host.write_stream_open = true;
    detect_gpio_level = 0u;
    assert(!pf_sdio_pio_is_initialized());
    pf_sdio_pio_confirm_card_removed();
    assert(!sdio_pio_host.write_stream_open && forgotten_removed_cards == 1u);
    assert(closes == previous_closes + 2u); /* confirmed removal sent no stop */
    detect_gpio_level = 1u;
    assert(initialize() == PF_SD_CARD_OK);
    assert(card_initialized && !sdio_pio_host.write_stream_open);
    assert(forgotten_removed_cards == 1u);
    reset();
    active_config.use_card_detect = true;
    detect_gpio_level = 0u;
    assert(initialize() == PF_SD_CARD_INIT_FAILED && closes == 0u);
    detect_gpio_level = 1u;
    assert(initialize() == PF_SD_CARD_OK && card_initialized);
    assert(!sdio_pio_host.write_stream_open && closes == 1u);
    uint32_t max_blocks_32 = UINT32_MAX / PF_SD_CARD_BLOCK_SIZE;
    assert(validate_transfer_32(0u, buffer, max_blocks_32, UINT32_MAX) == PF_SD_CARD_OK);
    assert(validate_transfer_32(0u, buffer, max_blocks_32 + 1u, UINT32_MAX)
        == PF_SD_CARD_INVALID_ARGUMENT);
    assert(validate_transfer_32(UINT32_MAX, NULL, 0u, 0u) == PF_SD_CARD_OK);
    puts("SDIO initialization and transfer routing checks passed");
}
"""


def main():
    check_optional_variants()
    source = SOURCE.read_text()
    probe = function_source(source, "static bool probe_transfer(void)")
    read_retry = function_source(source, "static bool read_blocks_with_clock_fallback(")
    initialized = "\n".join(function_source(source, signature) for signature in (
        "static bool card_detect_present(void)",
        "bool pf_sdio_pio_is_initialized(void)",
        "void pf_sdio_pio_confirm_card_removed(void)",
        "pf_sd_card_result_t pf_sdio_pio_sync(void)",
        "void pf_sdio_pio_get_info("))
    transfer = function_source(source, "static pf_sd_card_result_t transfer_blocks(")
    protocol = (SOURCE.parents[1] / "sd_card_protocol.c").read_text()
    validate_32 = function_source(protocol, "pf_sd_card_result_t pf_sd_card_validate_transfer(")
    validate_32 = validate_32.replace("pf_sd_card_validate_transfer", "validate_transfer_32")
    validate_32 = validate_32.replace("SIZE_MAX", "UINT32_MAX")
    init = function_source(source, "static bool initialize_card(")
    request = init[init.index("    uint32_t requested_clock"):init.index("    uint32_t transfer_clock")]
    retry = function_source(source, "pf_sd_card_result_t pf_sdio_pio_init(")
    ladder_start = source.index("static const uint32_t transfer_clock_steps_hz[]")
    ladder = source[ladder_start:source.index("};", ladder_start) + 2]
    selection = init[init.index("card_init_stage = PF_SDIO_PIO_INIT_STAGE_TRANSFER_CLOCK;"):-1]
    retry = retry[retry.index("    if (!card_detect_present())"):-1]
    code = (HARNESS.replace("@CONFIG_VALID@", function_source(source, "static bool config_is_valid("))
            .replace("@LADDER@", ladder).replace("@PROBE@", probe)
            .replace("@READ_RETRY@", read_retry)
            .replace("@IS_INITIALIZED@", initialized)
            .replace("@TRANSFER@", transfer)
            .replace("@VALIDATE_32@", validate_32)
            .replace("@REQUEST@", request)
            .replace("@SELECT@", selection).replace("@RETRY@", retry))
    with tempfile.TemporaryDirectory() as scratch:
        src = Path(scratch) / "check.c"
        exe = Path(scratch) / "check"
        src.write_text(code)
        subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                        "-I", str(SOURCE.parents[1]),
                        str(src), str(SOURCE.parents[1] / "sd_card_protocol.c"),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()