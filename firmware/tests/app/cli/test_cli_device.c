#include "unity.h"
#include "cli.h"
#include "embedded_cli.h"
#include <string.h>

#define PF_ENABLE_WIRELESS 1
#define PF_ENABLE_STORAGE_BENCHMARK 1
#define PF_FIRMWARE_VERSION_MAJOR 1u
#define PF_FIRMWARE_VERSION_MINOR 2u
#define PF_FIRMWARE_VERSION_PATCH 3u
#define DEVICE_SOURCE "app/cli/cli_device.c"
#include DEVICE_SOURCE

static char captured[8192];
static size_t captured_size, largest_response;
static pf_sysclk_profile_t clock_profile, saved_clock;
static pf_storage_speed_t speed_profile, saved_speed;
static uint8_t radio_flags, saved_radios;
static unsigned saves, discards, setters, reboots, led_changes, msc_changes;
static unsigned stage_failure_at;
static bool save_ok, stage_ok, worker_busy, host_owned, transition, led_available, led_on, msc_ok;
static bool benchmark_ok, cancel_ok;
static uint32_t benchmark_size;
static pf_file_benchmark_status_t benchmark;
static uint8_t radio_report[PF_WIRELESS_STATUS_SIZE];

pf_sysclk_profile_t pf_boot_config_sysclk(void) { return clock_profile; }
pf_storage_speed_t pf_boot_config_storage_speed(void) { return speed_profile; }
uint8_t pf_boot_config_radios(void) { return radio_flags; }
bool pf_boot_config_set_sysclk(pf_sysclk_profile_t value)
{ ++setters; if (!stage_ok || setters == stage_failure_at) return false; clock_profile = value; return true; }
bool pf_boot_config_set_storage_speed(pf_storage_speed_t value)
{ ++setters; if (!stage_ok || setters == stage_failure_at) return false; speed_profile = value; return true; }
bool pf_boot_config_set_radios(uint8_t value)
{ ++setters; if (!stage_ok || setters == stage_failure_at) return false; radio_flags = value; return true; }
bool pf_boot_config_save(void)
{
    ++saves;
    if (!save_ok) return false;
    saved_clock = clock_profile;
    saved_speed = speed_profile;
    saved_radios = radio_flags;
    return true;
}
void pf_boot_config_discard(void)
{ ++discards; clock_profile = saved_clock; speed_profile = saved_speed; radio_flags = saved_radios; }
bool pf_storage_worker_busy(void) { return worker_busy; }
bool pf_usb_msc_is_enabled(void) { return host_owned; }
bool pf_usb_msc_is_ejected(void) { return false; }
bool pf_usb_msc_transition_pending(void) { return transition; }
bool pf_usb_msc_request_rejected(void) { return !msc_ok; }
bool pf_usb_msc_request(bool enabled) { ++msc_changes; host_owned = enabled; return msc_ok; }
bool pf_led_is_on(void) { return led_on; }
bool pf_led_set(bool on) { ++led_changes; if (!led_available) return false; led_on = on; return true; }
uint32_t pf_system_clock_hz(void) { return 125000000u; }
uint32_t pf_system_uptime_ms(void) { return 123u; }
bool pf_storage_worker_benchmark_start(uint32_t size_mib, bool owned)
{ TEST_ASSERT_FALSE(owned); benchmark_size = size_mib; return benchmark_ok; }
bool pf_storage_worker_benchmark_cancel(void) { return cancel_ok; }
void pf_storage_worker_benchmark_status(pf_file_benchmark_status_t *status) { *status = benchmark; }
void pf_wireless_status(uint8_t *report) { memcpy(report, radio_report, sizeof(radio_report)); }
static unsigned recoveries, worker_queries;
void pf_usb_msc_request_recovery(void) { ++recoveries; }

static void request_reboot(void) { ++reboots; }
static void capture(char character, void *context)
{
    (void)context;
    TEST_ASSERT_LESS_THAN(sizeof(captured) - 1u, captured_size);
    captured[captured_size++] = character;
    captured[captured_size] = '\0';
}
static void send(const char *command)
{
    captured_size = 0u;
    captured[0] = '\0';
    while (*command != '\0') {
        const size_t before = captured_size;
        pf_cli_receive_char(*command++);
        pf_cli_process();
        const size_t response = captured_size - before;
        if (response > largest_response) largest_response = response;
        TEST_ASSERT_LESS_OR_EQUAL_UINT(1024u, response);
    }
}

/** Mirrors usb_task's storage binding: synchronous verbs first, status queued to the worker. */
static void storage(EmbeddedCli *instance, char *args, void *context)
{
    (void)instance;
    (void)context;
    if (!pf_cli_device_storage(args)) ++worker_queries;
}

void setUp(void)
{
    captured_size = largest_response = 0u;
    clock_profile = saved_clock = PF_SYSCLK_DEFAULT;
    speed_profile = saved_speed = PF_STORAGE_SPEED_DEFAULT;
    radio_flags = saved_radios = PF_WIRELESS_WIFI | PF_WIRELESS_BLE;
    saves = discards = setters = reboots = led_changes = msc_changes = recoveries = worker_queries = 0u;
    stage_failure_at = 0u;
    worker_busy = host_owned = transition = led_on = false;
    save_ok = stage_ok = led_available = msc_ok = benchmark_ok = cancel_ok = true;
    benchmark_size = 0u;
    memset(&benchmark, 0, sizeof(benchmark));
    memset(radio_report, 0, sizeof(radio_report));
    TEST_ASSERT_TRUE(pf_cli_init(capture, NULL));
    TEST_ASSERT_TRUE(pf_cli_device_register(request_reboot));
    TEST_ASSERT_TRUE(pf_cli_add_binding((CliCommandBinding){ "storage", PF_CLI_STORAGE_HELP, false, NULL, storage }));
    pf_cli_process();
}

void tearDown(void) {}

static void noop(EmbeddedCli *instance, char *args, void *context)
{ (void)instance; (void)args; (void)context; }

void test_help_with_every_production_binding_fits_output_reserve(void)
{
    TEST_ASSERT_TRUE(pf_cli_add_binding((CliCommandBinding){
        "fs", "ls [path]\r\n\tstat <path>\r\n\tcat <path> [limit]\r\n\tdf\r\n"
              "\tmkdir <path>\r\n\twrite <path> <text>\r\n\tappend <path> <text>\r\n"
              "\tmv <source> <destination>\r\n\trm <path> --confirm", false, NULL, noop
    }));
    send("help\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, " * config\r\n\tget [clock | storage | wifi | bluetooth]"));
    TEST_ASSERT_NOT_NULL(strstr(captured, " * storage\r\n\tget [status | usb | benchmark]\r\n\tset usb <on | off>"));
    TEST_ASSERT_NULL(strstr(captured, " * msc"));
    TEST_ASSERT_NULL(strstr(captured, " * benchmark"));
    TEST_ASSERT_NOT_NULL(strstr(captured, "\tformat --confirm (destroys files)\r\n\tmount (maintenance)"));
    TEST_ASSERT_NOT_NULL(strstr(captured, " * fs\r\n\tls [path]"));
    TEST_ASSERT_LESS_OR_EQUAL_UINT(1024u, largest_response);
    send("help config\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "key=value"));
    TEST_ASSERT_NOT_NULL(strstr(captured, "(saves and reboots)"));
    send("board --help\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "\tget <clock | led | version | uptime>\r\n\tset led <on | off>\r\n\treboot"));
    TEST_ASSERT_EQUAL_UINT(0u, reboots);
}

void test_board_reads_actual_clock_led_uptime_and_build_version(void)
{
    send("board get clock\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "clock=125000000 Hz"));
    send("board get version\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "version=1.2.3"));
    send("board get uptime\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "uptime=123 ms"));
    send("board set led on\r");
    TEST_ASSERT_TRUE(led_on);
    send("board get led\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "led=on"));
    send("board set led off\r");
    TEST_ASSERT_FALSE(led_on);
}

void test_board_invalid_requests_have_no_side_effects(void)
{
    send("board set led on extra\r");
    send("board set led maybe\r");
    send("board reboot extra\r");
    send("board\r");
    TEST_ASSERT_EQUAL_UINT(0u, led_changes);
    TEST_ASSERT_EQUAL_UINT(0u, reboots);
    led_available = false;
    send("board get led\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "led=off"));
    send("board set led on\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "ERR LED unavailable"));
}

void test_config_reads_saved_targets_not_running_clock(void)
{
    clock_profile = PF_SYSCLK_250MHZ;
    send("config get\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "clock=250 MHz (next boot)"));
    TEST_ASSERT_NOT_NULL(strstr(captured, "storage=default"));
    TEST_ASSERT_NOT_NULL(strstr(captured, "bluetooth=enabled"));
    TEST_ASSERT_NULL(strstr(captured, "125000000"));
    send("config get missing\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "ERR config get"));
    TEST_ASSERT_EQUAL_UINT(0u, setters);
}

void test_config_validates_all_assignments_then_saves_and_requests_reboot(void)
{
    send("config set clock = 200, storage=high, wifi=disabled, bluetooth=enabled\r");
    TEST_ASSERT_EQUAL_UINT(PF_SYSCLK_200MHZ, saved_clock);
    TEST_ASSERT_EQUAL_UINT(PF_STORAGE_SPEED_HIGH, saved_speed);
    TEST_ASSERT_EQUAL_UINT(PF_WIRELESS_BLE, saved_radios);
    TEST_ASSERT_EQUAL_UINT(1u, saves);
    TEST_ASSERT_EQUAL_UINT(1u, reboots);
    TEST_ASSERT_NOT_NULL(strstr(captured, "OK config saved; rebooting"));
}

void test_config_rejects_unknown_duplicate_empty_and_out_of_range_assignments_before_staging(void)
{
    static const char *const requests[] = {
        "config set clock=200,unknown=x\r", "config set clock=150,clock=200\r",
        "config set storage=high,\r", "config set clock=125\r", "config set clock=+150\r",
        "config set storage=50\r", "config set wifi=maybe\r", "config set clock=\r",
        "config set password=secret\r", "config set =150\r", "config set\r",
        "config set clock=150,,storage=high\r"
    };
    for (size_t i = 0u; i < sizeof(requests) / sizeof(requests[0]); ++i) {
        send(requests[i]);
        TEST_ASSERT_NOT_NULL(strstr(captured, "ERR"));
    }
    TEST_ASSERT_EQUAL_UINT(0u, setters);
    TEST_ASSERT_EQUAL_UINT(0u, saves);
    TEST_ASSERT_EQUAL_UINT(0u, reboots);
}

void test_config_stage_and_save_failures_discard_targets_without_reboot(void)
{
    stage_ok = false;
    send("config set clock=200\r");
    TEST_ASSERT_EQUAL_UINT(1u, discards);
    TEST_ASSERT_EQUAL_UINT(0u, saves);
    stage_ok = true;
    save_ok = false;
    send("config set clock=250,storage=high\r");
    TEST_ASSERT_EQUAL_UINT(2u, discards);
    TEST_ASSERT_EQUAL_UINT(PF_SYSCLK_DEFAULT, clock_profile);
    TEST_ASSERT_EQUAL_UINT(PF_STORAGE_SPEED_DEFAULT, speed_profile);
    TEST_ASSERT_EQUAL_UINT(0u, reboots);
    TEST_ASSERT_NOT_NULL(strstr(captured, "ERR save unconfirmed"));
}

void test_config_partial_staging_failure_restores_every_target(void)
{
    for (unsigned failure = 2u; failure <= 3u; ++failure) {
        setters = 0u;
        stage_failure_at = failure;
        send("config set clock=250,storage=high,wifi=disabled\r");
        TEST_ASSERT_EQUAL_UINT(PF_SYSCLK_DEFAULT, clock_profile);
        TEST_ASSERT_EQUAL_UINT(PF_STORAGE_SPEED_DEFAULT, speed_profile);
        TEST_ASSERT_EQUAL_UINT(PF_WIRELESS_WIFI | PF_WIRELESS_BLE, radio_flags);
    }
    TEST_ASSERT_EQUAL_UINT(2u, discards);
    TEST_ASSERT_EQUAL_UINT(0u, saves);
    TEST_ASSERT_EQUAL_UINT(0u, reboots);
}

void test_config_rejects_assignment_buffer_limit_without_applying_a_truncated_request(void)
{
    char command[128] = "config set clock=150";
    const size_t length = strlen(command);
    memset(command + length, ' ', 111u - length);
    command[111] = '\r';
    command[112] = '\0';
    send(command);
    TEST_ASSERT_NOT_NULL(strstr(captured, "ERR configuration assignments"));
    TEST_ASSERT_EQUAL_UINT(0u, setters);
    TEST_ASSERT_EQUAL_UINT(0u, reboots);
}

void test_config_and_reboot_refuse_busy_usb_ownership_and_transitions(void)
{
    for (unsigned i = 0u; i < 3u; ++i) {
        worker_busy = i == 0u;
        host_owned = i == 1u;
        transition = i == 2u;
        send("config set clock=200\r");
        TEST_ASSERT_NOT_NULL(strstr(captured, "ERR storage busy"));
        send("board reboot\r");
        TEST_ASSERT_NOT_NULL(strstr(captured, "ERR storage busy"));
    }
    TEST_ASSERT_EQUAL_UINT(0u, setters);
    TEST_ASSERT_EQUAL_UINT(0u, reboots);
    worker_busy = host_owned = transition = false;
    send("board reboot\r");
    TEST_ASSERT_EQUAL_UINT(1u, reboots);
}

void test_storage_usb_distinguishes_request_admission_from_completion_and_refusal(void)
{
    send("storage set usb on\r");
    TEST_ASSERT_TRUE(host_owned);
    TEST_ASSERT_NOT_NULL(strstr(captured, "request queued"));
    send("storage get usb\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "usb=enabled"));
    send("storage set usb off\r");
    TEST_ASSERT_FALSE(host_owned);
    worker_busy = true;
    send("storage set usb on\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "ERR storage busy"));
    worker_busy = false;
    msc_ok = false;
    send("storage set usb on\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "ERR USB request refused"));
    send("storage set usb on extra\r");
    send("storage set usb maybe\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "ERR storage get [status"));
    send("msc enable\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "Unknown command"));
    TEST_ASSERT_EQUAL_UINT(3u, msc_changes);
}

void test_storage_status_is_queued_and_recovery_requires_confirmation(void)
{
    send("storage get\r");
    send("storage get status\r");
    TEST_ASSERT_EQUAL_UINT(2u, worker_queries);
    send("storage get status extra\r");
    send("storage recover\r");
    send("storage\r");
    TEST_ASSERT_EQUAL_UINT(2u, worker_queries);
    TEST_ASSERT_EQUAL_UINT(0u, recoveries);
    send("storage recover --confirm\r");
    TEST_ASSERT_EQUAL_UINT(1u, recoveries);
    TEST_ASSERT_NOT_NULL(strstr(captured, "not clean eject"));
}

void test_storage_maintenance_queues_only_complete_confirmed_commands(void)
{
    send("storage mount\r");
    send("storage unmount\r");
    send("storage format --confirm\r");
    TEST_ASSERT_EQUAL_UINT(3u, worker_queries);
    send("storage format\r");
    send("storage format --confirm extra\r");
    send("storage mount extra\r");
    send("storage unmount extra\r");
    TEST_ASSERT_EQUAL_UINT(3u, worker_queries);
    TEST_ASSERT_NOT_NULL(strstr(captured, "ERR storage"));
    TEST_ASSERT_EQUAL_UINT(0u, msc_changes);
    TEST_ASSERT_EQUAL_UINT(0u, recoveries);
}

void test_benchmark_start_size_boundaries_rejection_and_cancellation(void)
{
    send("storage benchmark 1\r");
    TEST_ASSERT_EQUAL_UINT(1u, benchmark_size);
    TEST_ASSERT_NOT_NULL(strstr(captured, "not yet admitted"));
    send("storage benchmark 1024\r");
    TEST_ASSERT_EQUAL_UINT(1024u, benchmark_size);
    benchmark_size = 0u;
    send("storage benchmark 1025\r");
    send("storage benchmark 4294967296\r");
    send("storage benchmark -1\r");
    send("storage benchmark 1x\r");
    send("storage benchmark 0\r");
    TEST_ASSERT_EQUAL_UINT(0u, benchmark_size);
    host_owned = true;
    send("storage benchmark 1\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "ERR USB owns"));
    host_owned = false;
    benchmark_ok = false;
    benchmark.request_error = PF_FILE_BENCHMARK_ERROR_SPACE;
    send("storage benchmark 1\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "ERR benchmark request refused"));
    TEST_ASSERT_NOT_NULL(strstr(captured, "request_error=6"));
    send("storage benchmark cancel\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "wait for cleanup"));
    cancel_ok = false;
    send("storage benchmark cancel\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "ERR no cancellable run"));
}

void test_benchmark_status_reports_failure_and_cleanup_details(void)
{
    benchmark.state = PF_FILE_BENCHMARK_FAILED;
    benchmark.error = PF_FILE_BENCHMARK_ERROR_VERIFY;
    benchmark.flags = PF_FILE_BENCHMARK_FILE_REMAINS;
    benchmark.mismatch_count = 7u;
    benchmark.written_bytes = 1024u;
    benchmark.read_bytes = 512u;
    send("storage get benchmark\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "benchmark=failed"));
    TEST_ASSERT_NOT_NULL(strstr(captured, "mismatches=7"));
    TEST_ASSERT_NOT_NULL(strstr(captured, "error=9"));
    TEST_ASSERT_NOT_NULL(strstr(captured, "flags=1"));
}

void test_radio_status_uses_sanitized_state_and_never_prints_ssid_or_secrets(void)
{
    radio_report[4] = 2u;
    radio_report[5] = 192u;
    radio_report[6] = 168u;
    radio_report[7] = 66u;
    radio_report[8] = 87u;
    memcpy(radio_report + 9u, "\x1b[2Jprivate-ssid", 16u);
    radio_report[42] = radio_report[43] = radio_report[46] = 1u;
    radio_report[45] = 2u;
    send("radio status\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "wifi=connected ip=192.168.66.87\r\nbluetooth=connected\r\n"));
    TEST_ASSERT_NULL(strstr(captured, "configured"));
    TEST_ASSERT_NULL(strstr(captured, "private-ssid"));
    send("radio missing\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "ERR radio status"));
}

void test_device_registration_rejects_missing_reset_scheduler(void)
{
    TEST_ASSERT_FALSE(pf_cli_device_register(NULL));
}
