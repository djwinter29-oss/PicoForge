#include "unity.h"
#include <string.h>
#define WORKER_SOURCE "app/storage/storage_worker.c"
#define HID_SOURCE "app/usb/usb_hid.c"
#define USB_TASK_SOURCE "app/usb/usb_task.c"
#include WORKER_SOURCE
#include HID_SOURCE
#include USB_TASK_SOURCE

static bool unavailable, reserved, initialized, mounted, queue_fails, retained_cid;
static unsigned queries, queued, recovery_requests, normal_submissions, messages;
bool pf_sd_try_lock(void)
{
    TEST_ASSERT_FALSE(reserved);
    reserved = !unavailable;
    return reserved;
}
void pf_sd_unlock(void) { TEST_ASSERT_TRUE(reserved); reserved = false; }
bool pf_sd_is_initialized(void) { TEST_ASSERT_TRUE(reserved); return initialized; }
bool pf_fatfs_volume_is_mounted(void) { TEST_ASSERT_TRUE(reserved); return mounted; }
void pf_sd_get_info(pf_sd_info_t *info)
{
    TEST_ASSERT_TRUE(reserved);
    ++queries;
    *info = (pf_sd_info_t){
        .initialized = initialized, .presence_known = true, .present = true,
        .cid_valid = initialized || retained_cid, .bus_clock_hz = 25000000, .block_count = 128
    };
    if (info->cid_valid) {
        memset(info->cid, 0x5a, sizeof(info->cid));
    }
}
bool pf_board_get_sd_card_config(pf_sd_config_t *config)
{
    config->transport.mode = PF_STORAGE_BUS_NATIVE_SD;
    return true;
}
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout)
{
    const storage_request_t *request = item;
    TEST_ASSERT_EQUAL_PTR(requests, queue);
    TEST_ASSERT_EQUAL_UINT32(0, timeout);
    if (request->test_size_mib == 0) {
        TEST_ASSERT_FALSE(request->host_owned);
        TEST_ASSERT_EQUAL_UINT32(cli_session, request->session);
        ++normal_submissions;
        busy = false; /* Model completion before the next CLI request. */
        return pdPASS;
    }
    TEST_ASSERT_EQUAL_UINT32(32, request->test_size_mib);
    if (queue_fails) return 0;
    ++queued;
    return pdPASS;
}
void pf_usb_msc_request_recovery(void) { ++recovery_requests; }
bool pf_usb_msc_is_enabled(void) { return false; }
bool pf_usb_msc_transition_pending(void) { return false; }
bool pf_usb_msc_request_rejected(void) { return false; }
void pf_cli_print(const char *message)
{ TEST_ASSERT_NOT_NULL(message); TEST_ASSERT_NOT_EQUAL(0, *message); ++messages; }
uint8_t pf_system_mcu_id(void) { return PF_SYSTEM_MCU_RP2040; }
uint32_t pf_system_clock_hz(void) { return 125000000u; }
uint32_t pf_system_uptime_ms(void) { return 123u; }
void pf_runtime_stats_read(pf_runtime_stats_t *stats)
{
    *stats = (pf_runtime_stats_t){.cpu_usage_basis_points = 1234u,
        .ram_used_bytes = 65536u, .ram_total_bytes = 532480u};
}
bool tud_mounted(void) { return true; }
bool tud_cdc_connected(void) { return true; }
bool pf_temperature_read_centidegrees(int16_t *temperature)
{ *temperature = 2500; return true; }
bool pf_led_is_available(void) { return true; }
bool pf_led_is_on(void) { return false; }
void pico_get_unique_board_id_string(char *id, size_t size)
{ memset(id, 0, size); }
