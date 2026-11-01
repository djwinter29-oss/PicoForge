static char captured[8192];
static size_t captured_size, largest_response;
static pf_sysclk_profile_t clock_profile, saved_clock;
static pf_storage_speed_t speed_profile, saved_speed;
static uint8_t radio_flags, saved_radios;
static unsigned saves, discards, setters, reboots, led_changes;
static unsigned stage_failure_at;
static bool save_ok, stage_ok, worker_busy, host_owned, transition, led_on;
static uint8_t radio_report[PF_WIRELESS_STATUS_SIZE];

static pf_sysclk_profile_t mock_sysclk(int calls)
{
    (void)calls;
    return clock_profile;
}
static pf_storage_speed_t mock_storage_speed(int calls)
{
    (void)calls;
    return speed_profile;
}
static uint8_t mock_radios(int calls)
{
    (void)calls;
    return radio_flags;
}
static bool mock_set_sysclk(pf_sysclk_profile_t value, int calls)
{
    (void)calls;
    ++setters;
    if (!stage_ok || setters == stage_failure_at) return false;
    clock_profile = value;
    return true;
}
static bool mock_set_storage_speed(pf_storage_speed_t value, int calls)
{
    (void)calls;
    ++setters;
    if (!stage_ok || setters == stage_failure_at) return false;
    speed_profile = value;
    return true;
}
static bool mock_set_radios(uint8_t value, int calls)
{
    (void)calls;
    ++setters;
    if (!stage_ok || setters == stage_failure_at) return false;
    radio_flags = value;
    return true;
}
static bool mock_save(int calls)
{
    (void)calls;
    ++saves;
    if (!save_ok) return false;
    saved_clock = clock_profile;
    saved_speed = speed_profile;
    saved_radios = radio_flags;
    return true;
}
static void mock_discard(int calls)
{
    (void)calls;
    ++discards;
    clock_profile = saved_clock;
    speed_profile = saved_speed;
    radio_flags = saved_radios;
}
bool pf_storage_worker_busy(void) { return worker_busy; }
bool pf_led_is_on(void) { return led_on; }
bool pf_led_set(bool on) { ++led_changes; led_on = on; return true; }
uint32_t pf_system_clock_hz(void) { return 125000000u; }
uint32_t pf_system_uptime_ms(void) { return 123u; }
static void request_reboot(void) { ++reboots; }
static void mock_wireless_status(uint8_t *report, int calls)
{
    (void)calls;
    memcpy(report, radio_report, sizeof(radio_report));
}
static bool mock_msc_enabled(int calls)
{
    (void)calls;
    return host_owned;
}
static bool mock_msc_transition(int calls)
{
    (void)calls;
    return transition;
}


static bool queue_command(const char *family, const char *arguments, void *context)
{
    (void)family;
    (void)arguments;
    (void)context;
    return true;
}

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

static void device_set_up(void)
{
    captured_size = largest_response = 0u;
    clock_profile = saved_clock = PF_SYSCLK_DEFAULT;
    speed_profile = saved_speed = PF_STORAGE_SPEED_DEFAULT;
    radio_flags = saved_radios = PF_WIRELESS_WIFI | PF_WIRELESS_BLE;
    saves = discards = setters = reboots = led_changes = 0u;
    stage_failure_at = 0u;
    worker_busy = host_owned = transition = led_on = false;
    save_ok = stage_ok = true;
    memset(radio_report, 0, sizeof(radio_report));
    Mockboot_config_Init();
    Mockusb_msc_Init();
    Mockwireless_Init();
    pf_boot_config_sysclk_StubWithCallback(mock_sysclk);
    pf_boot_config_storage_speed_StubWithCallback(mock_storage_speed);
    pf_boot_config_radios_StubWithCallback(mock_radios);
    pf_boot_config_set_sysclk_StubWithCallback(mock_set_sysclk);
    pf_boot_config_set_storage_speed_StubWithCallback(mock_set_storage_speed);
    pf_boot_config_set_radios_StubWithCallback(mock_set_radios);
    pf_boot_config_save_StubWithCallback(mock_save);
    pf_boot_config_discard_StubWithCallback(mock_discard);
    pf_usb_msc_is_enabled_StubWithCallback(mock_msc_enabled);
    pf_usb_msc_transition_pending_StubWithCallback(mock_msc_transition);
    pf_wireless_status_StubWithCallback(mock_wireless_status);
    TEST_ASSERT_TRUE(pf_cli_init(capture, NULL));
    TEST_ASSERT_TRUE(pf_cli_board_register(request_reboot));
    TEST_ASSERT_TRUE(pf_cli_config_register(request_reboot));
    TEST_ASSERT_TRUE(pf_cli_storage_register(queue_command, NULL));
    TEST_ASSERT_TRUE(pf_cli_fs_register(queue_command, NULL));
    pf_cli_process();
}

static void device_tear_down(void)
{
    Mockboot_config_Verify();
    Mockboot_config_Destroy();
    Mockusb_msc_Verify();
    Mockusb_msc_Destroy();
    Mockwireless_Verify();
    Mockwireless_Destroy();
}
