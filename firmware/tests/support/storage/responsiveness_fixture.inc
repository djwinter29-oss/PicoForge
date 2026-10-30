#include "unity.h"
#include <string.h>
#define WORKER_SOURCE "app/storage/storage_worker.c"
#define HID_SOURCE "app/usb/usb_hid.c"
#define USB_TASK_SOURCE "app/usb/usb_task.c"
#include WORKER_SOURCE
#include HID_SOURCE
#include USB_TASK_SOURCE
static pf_sysclk_profile_t test_sysclk, saved_sysclk;
static pf_storage_speed_t test_sd_speed, saved_sd_speed;
static unsigned settings_save_calls;
static bool settings_save_fails;
static bool msc_enabled;
static bool msc_transition_pending;
static bool prepare_save_fails;
static unsigned prepare_save_calls;
bool pf_usb_msc_prepare_save(void) { ++prepare_save_calls; return !prepare_save_fails; }
void pf_usb_msc_finish_save(bool saved) { msc_enabled = !saved; }
void pf_usb_msc_task(void) {}
bool pf_logging_write(pf_logging_level_t level, const char *tag, const char *format, ...)
{ (void)level; (void)tag; (void)format; return true; }
const pf_led_board_t pf_led_board = {.gpio = 25u, .active_high = true};
const pf_logging_board_t pf_logging_board = {.uart_index = 0u, .tx_gpio = 0u, .rx_gpio = 1u};

pf_sysclk_profile_t pf_boot_config_sysclk(void) { return test_sysclk; }
pf_storage_speed_t pf_boot_config_storage_speed(void) { return test_sd_speed; }

bool pf_boot_config_set_sysclk(pf_sysclk_profile_t profile)
{
    if ((unsigned)profile > PF_SYSCLK_250MHZ) {
        return false;
    }
    test_sysclk = profile;
    return true;
}

bool pf_boot_config_set_storage_speed(pf_storage_speed_t speed)
{
    if (speed != PF_STORAGE_SPEED_DEFAULT && speed != PF_STORAGE_SPEED_HIGH) {
        return false;
    }
    test_sd_speed = speed;
    return true;
}

bool pf_boot_config_save(void)
{
    ++settings_save_calls;
    if (settings_save_fails) {
        return false;
    }
    saved_sysclk = test_sysclk;
    saved_sd_speed = test_sd_speed;
    return true;
}

static bool unavailable, reserved, initialized, mounted, queue_fails, retained_cid;
static TickType_t fake_ticks;
static bool fake_connected = true;
static bool fake_done;
static unsigned reply_count, drained_replies;
static uint32_t reply_session_skew;
static unsigned queries, queued, recovery_requests, normal_submissions, messages;
bool pf_fatfs_volume_is_mounted(void) { TEST_ASSERT_TRUE(reserved); return mounted; }
void pf_storage_get_status(pf_storage_status_t *status)
{
    TEST_ASSERT_FALSE(reserved);
    *status = (pf_storage_status_t){
        .medium = PF_STORAGE_MEDIUM_SDCARD, .bus = PF_STORAGE_BUS_SDIO,
        .bus_gpios = {2u, 3u, 4u, 5u, 6u, 7u}, .detect_gpio = 8u, .state = PF_STORAGE_STATE_BUSY,
    };
    if (unavailable) return;
    reserved = true;
    ++queries;
    status->presence_known = true;
    status->id_valid = initialized || retained_cid;
    if (status->id_valid) memset(status->id, 0x5a, sizeof(status->id));
    status->bus_clock_hz = 25000000u;
    status->sector_count = 128u;
    status->state = !initialized ? PF_STORAGE_STATE_UNINITIALIZED
        : pf_fatfs_volume_is_mounted() ? PF_STORAGE_STATE_MOUNTED : PF_STORAGE_STATE_READY;
    reserved = false;
}
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout)
{
    if (queue == replies) {
        TEST_ASSERT_EQUAL_UINT32(50u, timeout);
        TEST_ASSERT_EQUAL_UINT(8u, reply_count);
        fake_ticks += timeout;
        cdc_cli_task();
        return 0;
    }
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
bool pf_usb_msc_is_enabled(void) { return msc_enabled; }
bool pf_usb_msc_transition_pending(void) { return msc_transition_pending; }
bool pf_usb_msc_request_rejected(void) { return false; }
void pf_usb_msc_defer_for_hid_result(uint8_t transaction_id) { (void)transaction_id; }
void pf_usb_msc_note_hid_result_read(uint8_t transaction_id) { (void)transaction_id; }
void pf_cli_print(const char *message)
{ TEST_ASSERT_NOT_NULL(message); TEST_ASSERT_NOT_EQUAL(0, *message); ++messages; }
uint8_t pf_system_mcu_id(void) { return PF_SYSTEM_MCU_RP2040; }
uint32_t pf_system_clock_hz(void) { return 125000000u; }
uint8_t pf_system_reset_cause(void) { return 2u; }
uint32_t pf_system_uptime_ms(void) { return 123u; }
void pf_runtime_stats_read(pf_runtime_stats_t *stats)
{
    *stats = (pf_runtime_stats_t){.cpu_usage_basis_points = {1234u, 5678u},
        .ram_used_bytes = 65536u, .ram_total_bytes = 532480u};
}
bool tud_mounted(void) { return true; }
bool tud_cdc_connected(void) { return fake_connected; }
bool pf_temperature_read_centidegrees(int16_t *temperature)
{ *temperature = 2500; return true; }
bool pf_led_is_available(void) { return true; }
bool pf_led_is_on(void) { return false; }
void pico_get_unique_board_id(pico_unique_board_id_t *id)
{ for (size_t i = 0; i < sizeof(id->id); ++i) id->id[i] = (uint8_t)i; }
void pico_get_unique_board_id_string(char *id, size_t size)
{ memset(id, 0, size); }
absolute_time_t get_absolute_time(void) { return 1230000u; }
uint64_t to_ms_since_boot(absolute_time_t value) { return value / 1000u; }
uint64_t time_us_64(void) { return 1230000u + (uint64_t)fake_ticks * 1000u; }

TickType_t xTaskGetTickCount(void) { return fake_ticks; }
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout)
{
    TEST_ASSERT_EQUAL_PTR(replies, queue);
    TEST_ASSERT_EQUAL_UINT(0u, timeout);
    if (!reply_count) return 0;
    pf_storage_worker_reply_t *reply = item;
    *reply = (pf_storage_worker_reply_t){ .session = cli_session + reply_session_skew, .line = "reply" };
    --reply_count;
    ++drained_replies;
    reply->done = fake_done && reply_count == 0;
    return pdPASS;
}
uint32_t tud_cdc_write_available(void) { return 0; }
uint32_t tud_cdc_write(const void *buffer, uint32_t size) { (void)buffer; (void)size; return 0; }
uint32_t tud_cdc_write_flush(void) { return 0; }
uint32_t tud_cdc_available(void) { return 0; }
uint32_t tud_cdc_read(void *buffer, uint32_t size) { (void)buffer; (void)size; return 0; }
void tud_cdc_read_flush(void) {}
bool tud_cdc_write_clear(void) { return true; }
bool pf_cli_init(pf_cli_write_char_t writer, void *context) { (void)writer; (void)context; return true; }
bool pf_cli_add_binding(CliCommandBinding binding) { (void)binding; return true; }
void pf_cli_process(void) {}
void pf_cli_receive_char(char character) { (void)character; }
void panic(const char *format, ...) { (void)format; TEST_FAIL_MESSAGE("Unexpected panic"); }
pf_fatfs_volume_filesystem_format_t pf_fatfs_volume_filesystem_format(void) { return PF_FATFS_VOLUME_FILESYSTEM_EXFAT; }

static unsigned led_requests, msc_requests, force_requests;
static unsigned reset_requests;
static unsigned reboot_usb_polls;
static pf_hid_control_result_t reboot_ack;
void vTaskDelay(TickType_t ticks) { fake_ticks += ticks; }
void tud_task_ext(uint32_t timeout_ms, bool in_isr)
{
    TEST_ASSERT_EQUAL_UINT32(0u, timeout_ms);
    TEST_ASSERT_FALSE(in_isr);
    TEST_ASSERT_EQUAL_UINT(0u, reset_requests);
    ++reboot_usb_polls;
    TEST_ASSERT_EQUAL_UINT16(sizeof(reboot_ack), tud_hid_get_report_cb(
        0u, PF_HID_CONTROL_REPORT_ID, HID_REPORT_TYPE_FEATURE, (uint8_t *)&reboot_ack, sizeof(reboot_ack)));
}
static bool requested_state;
bool pf_led_set(bool on) { ++led_requests; requested_state = on; return true; }
void pf_system_reset(void) { ++reset_requests; }
bool pf_usb_msc_request(bool enabled) { ++msc_requests; requested_state = enabled; return true; }
bool pf_usb_msc_request_force_return(void) { ++force_requests; requested_state = false; return true; }
