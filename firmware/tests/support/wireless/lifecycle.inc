/* Host radio model used by the wireless contract tests; run through pytest's contract selectors. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app/wireless/wifi/wifi.c"
#include "app/wireless/wireless.c"

cyw43_t cyw43_state;
static uint64_t now;
static int link, join_rc, leave_rc, scan_rc, init_rc;
static bool driver_scan, serving, auth_required, shared_key;
static unsigned joins, leaves, releases, zeroizes;
static uint32_t last_auth;
static uint8_t joined_ssid[32], joined_length;
static char joined_password[64];
static scan_result_cb_t scan_callback;
static uint8_t saved_ssid[32], saved_length, saved_key[32];
static bool saved_key_present;
static char saved_password[64];
static pf_wifi_auth_state_t saved_auth;
static uint8_t radios;
static bool cli_enabled;

void mbedtls_platform_zeroize(void *ptr, size_t count)
{
    volatile uint8_t *p = ptr;
    while (count-- != 0u) *p++ = 0u;
    ++zeroizes;
}
uint64_t time_us_64(void) { return now; }
int cyw43_arch_init(void) { return init_rc; }
void cyw43_arch_poll(void) {}
void cyw43_arch_gpio_put(unsigned pin, bool on) { (void)on; assert(pin == 0u); }
unsigned get_core_num(void) { return 0u; }
void vTaskCoreAffinitySet(void *task, UBaseType_t affinity) { assert(task == NULL && affinity == 1u); }
void cyw43_arch_enable_sta_mode(void) {}
void cyw43_arch_disable_sta_mode(void) { ++releases; link = CYW43_LINK_DOWN; }
bool cyw43_wifi_scan_active(cyw43_t *state) { assert(state == &cyw43_state); return driver_scan; }
int cyw43_wifi_scan(cyw43_t *state, cyw43_wifi_scan_options_t *options, void *env, scan_result_cb_t cb)
{
    assert(state == &cyw43_state && options != NULL && env == NULL);
    scan_callback = cb;
    driver_scan = scan_rc == 0;
    return scan_rc;
}
int cyw43_wifi_join(cyw43_t *state, size_t length, const uint8_t *ssid, size_t key_length,
                    const uint8_t *password, uint32_t auth, const uint8_t *bssid, uint32_t channel)
{
    assert(state == &cyw43_state && length <= 32u && key_length <= 63u);
    assert(bssid == NULL && channel == CYW43_CHANNEL_NONE);
    ++joins;
    joined_length = (uint8_t)length;
    memset(joined_ssid, 0, sizeof(joined_ssid));
    memcpy(joined_ssid, ssid, length);
    memset(joined_password, 0, sizeof(joined_password));
    memcpy(joined_password, password, key_length);
    last_auth = auth;
    return join_rc;
}
int cyw43_wifi_leave(cyw43_t *state, int itf)
{ assert(state == &cyw43_state && itf == 0); ++leaves; return leave_rc; }
int cyw43_tcpip_link_status(cyw43_t *state, int itf)
{ assert(state == &cyw43_state && itf == 0); return link; }
int cyw43_wifi_get_rssi(cyw43_t *state, int32_t *rssi)
{ assert(state == &cyw43_state); *rssi = -47; return 0; }
uint8_t pf_boot_config_radios(void) { return radios; }
uint8_t pf_boot_config_wifi_ssid_bytes(uint8_t out[32])
{ memcpy(out, saved_ssid, 32u); return saved_length; }
bool pf_boot_config_set_wifi_ssid_bytes(const uint8_t *ssid, uint8_t length)
{
    assert(length <= 32u);
    memset(saved_ssid, 0, 32u); memcpy(saved_ssid, ssid, length); saved_length = length; return true;
}
void pf_boot_config_wifi_password(char out[64]) { memcpy(out, saved_password, 64u); }
bool pf_boot_config_set_wifi_password(const char *password)
{ memset(saved_password, 0, 64u); strcpy(saved_password, password); return true; }
pf_wifi_auth_state_t pf_boot_config_wifi_auth_state(void) { return saved_auth; }
bool pf_boot_config_set_wifi_auth_state(pf_wifi_auth_state_t auth) { saved_auth = auth; return true; }
bool pf_boot_config_tcp_cli_enabled(void) { return cli_enabled; }
void pf_boot_config_ble_key(uint8_t out[32]) { memcpy(out, saved_key, 32u); }
bool pf_boot_config_ble_key_configured(void) { return saved_key_present; }
bool pf_boot_config_set_ble_key(const uint8_t key[32])
{
    memset(saved_key, 0, 32u);
    saved_key_present = key != NULL;
    if (key != NULL) memcpy(saved_key, key, 32u);
    return true;
}
static bool ble_prepared_with_key; /**< Key was loaded when Bluetooth evaluated its key gate. */
bool pf_bluetooth_prepare(void) { ble_prepared_with_key = shared_key; return false; }
bool pf_bluetooth_start(void) { return true; }
uint8_t pf_bluetooth_state(void) { return 0u; }
void pf_bluetooth_poll(void) {}
bool pf_tcp_cli_set_auth_required(bool required) { auth_required = required; return shared_key || !required; }
bool pf_tcp_cli_serving(void) { return serving; }
void pf_tcp_cli_poll(bool enabled, uint64_t timestamp) { assert(timestamp == now); serving = enabled; }
void pf_wifi_mdns_poll(bool saved, bool tcp, uint64_t timestamp)
{ assert(timestamp == now && (!tcp || saved)); }
void pf_wireless_auth_set_key(const uint8_t key[32])
{ shared_key = key != NULL; }
bool pf_wireless_auth_tcp_cli_proof(const uint8_t nonce[16], uint8_t proof[16])
{ if (!shared_key) return false; memcpy(proof, nonce, 16u); return true; }
bool pf_logging_write(pf_logging_level_t level, const char *tag, const char *format, ...)
{ (void)level; (void)tag; (void)format; return true; }

static void reset(void)
{
    pf_wireless_discard();
    started = wifi_enabled = ble_enabled = tcp_cli_enabled = boot_key_saved = false;
    init_error = 0u;
    station_enabled = saved_wifi_enabled = scan_release_pending = false;
    link_owner = PF_WIFI_LINK_NONE;
    after_leave = PF_WIFI_AFTER_LEAVE_NONE;
    scan_state = scan_count = test_generation = 0u;
    scan_generation = 0u;
    command_state = PF_WIFI_COMMAND_IDLE;
    wifi_state = PF_WIFI_STATE_DISABLED;
    wifi_error = PF_WIRELESS_ERROR_NONE;
    memset(saved_ssid, 0, 32u);
    memcpy(saved_ssid, "network", 7u); saved_length = 7u;
    strcpy(saved_password, "test-password");
    memset(saved_key, 0, 32u);
    saved_key_present = false;
    saved_auth = PF_WIFI_AUTH_TRY_WPA2;
    now = 0u;
    link = join_rc = leave_rc = scan_rc = init_rc = 0;
    driver_scan = serving = auth_required = shared_key = false;
    joins = leaves = releases = zeroizes = 0u;
    radios = PF_WIRELESS_WIFI;
    cli_enabled = true;
    assert(pf_wifi_prepare());
}

static pf_hid_control_result_t command(uint8_t op, uint8_t length, const void *value)
{
    pf_hid_control_request_t request = {.version = 1u, .transaction_id = 9u, .opcode = op};
    if (op <= 8u) {
        request.arguments[0] = length;
        if (length != 0u) memcpy(request.arguments + 1u, value, length);
    }
    pf_hid_control_result_t result;
    memset(&result, 0xa5, sizeof(result));
    pf_wireless_command(&request, &result);
    assert(result.version == 1u && result.transaction_id == 9u && result.opcode == op);
    return result;
}
static void ok(uint8_t op, uint8_t length, const void *value)
{ assert(command(op, length, value).status == 2u); }
static void rejected(uint8_t op, uint8_t status, uint8_t detail)
{
    pf_hid_control_result_t result = command(op, 0u, NULL);
    assert(result.status == status && result.result_data[0] == detail);
    assert(bytes_zero(result.result_data + 1u, 62u));
}
static void snapshot(uint8_t report[48])
{ memset(report, 0xa5, 48u); pf_wireless_status(report); assert(report[0] == 1u); }

static void radio_regressions(void)
{
    reset();
    radios = PF_WIRELESS_BLE; assert(!pf_wifi_prepare());
    radios = PF_WIRELESS_WIFI; saved_length = 0u; assert(!pf_wifi_prepare());
    /* A leading or embedded zero in a real SSID must not gate or truncate its join. */
    const uint8_t raw[] = {0, 0xff, 'a', 0, 'b'};
    pf_boot_config_set_wifi_ssid_bytes(raw, sizeof(raw));
    assert(pf_wifi_prepare()); pf_wifi_start();
    assert(joined_length == sizeof(raw) && memcmp(joined_ssid, raw, sizeof(raw)) == 0);
    assert(last_auth == CYW43_AUTH_WPA2_MIXED_PSK);
    now = 29999999u; pf_wifi_poll(now); assert(pf_wifi_state() == 1u && joins == 1u);
    ++now; pf_wifi_poll(now); assert(pf_wifi_state() == 3u && pf_wifi_error() == 3u);
    now += 4999999u; pf_wifi_poll(now); assert(joins == 1u);
    ++now; pf_wifi_poll(now); assert(joins == 2u && last_auth == CYW43_AUTH_WPA3_SAE_AES_PSK);
    link = CYW43_LINK_UP; pf_wifi_poll(now); assert(pf_wifi_saved_link_up());
    pf_wifi_auth_state_t auth;
    uint16_t reconnects;
    int8_t rssi;
    pf_wifi_link_diagnostics(&auth, &reconnects, &rssi);
    assert(auth == PF_WIFI_AUTH_SAE && reconnects == 0u && rssi == -47);
    uint8_t ip[] = {192, 168, 1, 42}, out[4];
    memcpy(&cyw43_state.netif[0].ip.addr, ip, 4u); pf_wifi_ip_address(out); assert(memcmp(ip, out, 4u) == 0);
    link = CYW43_LINK_DOWN; pf_wifi_poll(now); assert(!pf_wifi_saved_link_up());
    pf_wifi_link_diagnostics(&auth, &reconnects, &rssi);
    assert(auth == PF_WIFI_AUTH_SAE && reconnects == 1u && rssi == PF_WIFI_RSSI_UNAVAILABLE);
    now += 5000000u; join_rc = -1; pf_wifi_poll(now); assert(joins == 3u);
    now += 9999999u; join_rc = 0; pf_wifi_poll(now); assert(joins == 3u);
    ++now; pf_wifi_poll(now); assert(joins == 4u);

    reset(); radios = 0u; assert(!pf_wifi_prepare());
    assert(pf_wifi_test(raw, sizeof(raw), "password"));
    link = CYW43_LINK_NONET; pf_wifi_poll(now);
    assert(pf_wifi_test_active() && leaves == 1u);
    pf_wifi_poll(now); assert(last_auth == CYW43_AUTH_WPA3_SAE_AES_PSK);
    link = CYW43_LINK_UP; pf_wifi_poll(now);
    assert(pf_wifi_test_succeeded() && !pf_wifi_saved_link_up());
    assert(pf_wifi_tested_auth_state() == PF_WIFI_AUTH_SAE);
    uint8_t report[48] = {0}; pf_wifi_status(report); assert(report[1] == 5u && report[46] == 1u);
    link = CYW43_LINK_DOWN; pf_wifi_poll(now);
    pf_wifi_status(report); assert(report[1] == 6u && report[2] == 6u && pf_wifi_test_succeeded());
    pf_wifi_cancel_test(true); assert(!pf_wifi_test_succeeded());

    /* Repeated handshake loss is a rejected password, not a thirty-second timeout. */
    assert(pf_wifi_test(raw, sizeof(raw), "wrong-pass"));
    link = CYW43_LINK_JOIN;
    for (unsigned i = 0; i < 2u; ++i) {
        cyw43_state.wifi_join_state = PF_CYW43_JOIN_LINK; pf_wifi_poll(now);
        cyw43_state.wifi_join_state = 0u; pf_wifi_poll(now);
    }
    memset(report, 0, sizeof(report)); pf_wifi_status(report);
    assert(report[1] == 6u && report[2] == 3u && !pf_wifi_test_succeeded());
    assert(report[9] == sizeof(raw) && memcmp(report + 10u, raw, sizeof(raw)) == 0);
    link = CYW43_LINK_DOWN;
    assert(pf_wifi_test(raw, sizeof(raw), "password"));
    link = CYW43_LINK_NOIP; pf_wifi_poll(now);
    now += 30000000u; pf_wifi_poll(now); pf_wifi_status(report);
    assert(report[1] == 6u && report[2] == 4u);

    /* An old UP link cannot complete a new test before the disconnect is observed. */
    reset(); pf_wifi_start(); link = CYW43_LINK_UP; pf_wifi_poll(now);
    unsigned before = joins;
    assert(pf_wifi_test(raw, sizeof(raw), ""));
    pf_wifi_poll(now); assert(joins == before && pf_wifi_test_active());
    link = CYW43_LINK_DOWN; pf_wifi_poll(now);
    assert(joins == before + 1u && last_auth == CYW43_AUTH_OPEN);
    link = CYW43_LINK_UP; pf_wifi_poll(now); assert(pf_wifi_test_succeeded());
    leave_rc = -1; pf_wifi_cancel_test(true); before = leaves;
    now += 4999999u; pf_wifi_poll(now); assert(leaves == before);
    ++now; leave_rc = 0; pf_wifi_poll(now); assert(leaves == before + 1u);
    link = CYW43_LINK_DOWN; pf_wifi_poll(now); assert(pf_wifi_state() == 1u);
    link = CYW43_LINK_UP; pf_wifi_poll(now);
    before = leaves; pf_wifi_cancel_test(true); assert(leaves == before && pf_wifi_saved_link_up());

    /* Failed test restoration does not inflate the saved network's retry backoff. */
    for (unsigned attempt = 0u; attempt < 3u; ++attempt) {
        link = CYW43_LINK_DOWN;
        assert(pf_wifi_test(raw, sizeof(raw), "wrong-pass"));
        link = CYW43_LINK_BADAUTH; pf_wifi_poll(now);
        link = CYW43_LINK_DOWN; before = joins; pf_wifi_poll(now); assert(joins == before + 1u);
    }
    link = CYW43_LINK_BADAUTH; pf_wifi_poll(now); before = joins;
    now += 4999999u; pf_wifi_poll(now); assert(joins == before);
    ++now; pf_wifi_poll(now); assert(joins == before + 1u);
    uint8_t status[48] = {0};
    pf_wifi_cancel_test(true);
    link = CYW43_LINK_BADAUTH; pf_wifi_poll(now); pf_wifi_status(status);
    assert(status[1] == 3u && status[2] == 3u);
    reset(); radios = 0u; assert(!pf_wifi_prepare()); join_rc = -1;
    assert(pf_wifi_test(raw, sizeof(raw), "password"));
    pf_wifi_status(status);
    assert(status[1] == 6u && status[46] == 1u && !pf_wifi_test_succeeded());
    reset(); radios = 0u; assert(!pf_wifi_prepare());
    pf_wifi_scan(); driver_scan = false; pf_wifi_poll(now);
    assert(releases == 1u && !station_enabled); /* Scanning does not leave a disabled station enabled. */
    reset(); pf_wifi_start();
    assert(pf_wifi_test(raw, sizeof(raw), "password"));
    link = CYW43_LINK_UP; pf_wifi_poll(now); assert(pf_wifi_test_succeeded());
    leave_rc = -1;
    assert(!pf_wifi_test(raw, sizeof(raw), "password"));
    assert(!pf_wifi_test_succeeded() && after_leave == PF_WIFI_AFTER_LEAVE_RESTORE);
    now += 5000000u; leave_rc = 0; before = leaves; pf_wifi_poll(now);
    assert(leaves == before + 1u);
    link = CYW43_LINK_DOWN; pf_wifi_poll(now); assert(link_owner == PF_WIFI_LINK_SAVED);
}

static void scan_regressions(void)
{
    reset(); pf_wireless_start();
    ok(11u, 0u, NULL);
    uint8_t report[48]; snapshot(report); assert(report[42] == 1u && report[43] == 1u && report[45] == 0u);
    rejected(11u, 5u, 26u);
    rejected(9u, 5u, 26u); /* Busy before no-staged-values. */
    rejected(12u, 5u, 26u);
    ok(6u, 5u, "hello"); ok(10u, 0u, NULL);
    assert(driver_scan); /* Cancel discards candidates but does not stop scanning. */
    for (unsigned i = 0u; i < 20u; ++i) {
        cyw43_ev_scan_result_t ap = {.ssid_len = 2u, .rssi = (int16_t)(-100 + i), .auth_mode = 7u};
        ap.ssid[0] = 0u; ap.ssid[1] = (uint8_t)i;
        scan_callback(NULL, &ap);
    }
    cyw43_ev_scan_result_t duplicate = {.ssid_len = 2u, .ssid = {0u, 5u}, .rssi = -10, .auth_mode = 4u};
    scan_callback(NULL, &duplicate); scan_callback(NULL, &duplicate);
    driver_scan = false; pf_wireless_poll(); snapshot(report);
    assert(report[42] == 2u && report[45] == 18u && report[46] == 0u);
    uint8_t entry[63] = {0};
    assert(pf_wifi_scan_entry(0u, 1u, entry) == 0u);
    assert(entry[3] == 2u && entry[4] == 0u && entry[5] == 5u && (int8_t)entry[36] == -10 && entry[37] == 4u);
    assert(pf_wifi_scan_entry(18u, 1u, entry) == 23u);
    assert(pf_wifi_scan_entry(0u, 2u, entry) == 22u);
    pf_hid_control_request_t read = {.version = 1u, .transaction_id = 1u, .opcode = 12u};
    read.arguments[1] = 1u;
    pf_hid_control_result_t result; pf_wireless_command(&read, &result);
    assert(result.status == 2u && result.result_data[5] == 5u && bytes_zero(result.result_data + 38u, 25u));
    read.arguments[2] = 1u; pf_wireless_command(&read, &result); assert(result.result_data[0] == 22u);
    ok(11u, 0u, NULL); snapshot(report);
    assert(report[43] == 2u && report[45] == 0u);
    now += 10000000u; pf_wireless_poll(); snapshot(report); assert(report[42] == 3u && report[45] == 0u);
    scan_callback(NULL, &duplicate); snapshot(report); assert(report[45] == 0u);
    rejected(11u, 5u, 26u); /* Timed-out SDK scan must drain before reuse. */
    driver_scan = false; pf_wireless_poll();
    scan_generation = UINT16_MAX; scan_rc = -1; ok(11u, 0u, NULL);
    snapshot(report); assert(report[42] == 3u && report[43] == 1u && report[44] == 0u);
    /* A saved-link loss during scanning is reported immediately, but retries wait for SDK scan completion. */
    reset(); pf_wireless_start(); link = CYW43_LINK_UP; pf_wireless_poll();
    const unsigned before_scan = joins;
    ok(11u, 0u, NULL); link = CYW43_LINK_DOWN; pf_wireless_poll();
    assert(pf_wifi_state() == PF_WIFI_STATE_FAILED);
    now += 5000000u; pf_wireless_poll(); assert(joins == before_scan);
    now += 5000000u; pf_wireless_poll(); assert(joins == before_scan);
    driver_scan = false; pf_wireless_poll(); assert(joins == before_scan + 1u);
}

static void staging_regressions(void)
{
    reset(); pf_wireless_start();
    uint8_t report[48], mask;
    assert(!pf_wireless_staged() && pf_wireless_save_precondition(true) == 21u);
    rejected(9u, 3u, 21u); rejected(13u, 3u, 24u);
    uint8_t raw[32]; for (unsigned i = 0u; i < 32u; ++i) raw[i] = (uint8_t)(i * 13u);
    ok(6u, 32u, raw);
    assert(pf_wireless_staged() && pf_wireless_save_precondition(false) == 27u);
    assert(pf_wireless_save_precondition(true) == 19u);
    rejected(9u, 3u, 19u);
    /* Omitted passwords never leak from the saved network into a changed SSID. */
    ok(7u, 0u, NULL); assert(pf_wireless_save_precondition(true) == 0u);
    assert(pf_wireless_apply_staged(&mask) && mask == 3u);
    assert(saved_length == 32u && memcmp(saved_ssid, raw, 32u) == 0 && saved_password[0] == 0);
    assert(saved_auth == PF_WIFI_AUTH_TRY_WPA2);
    /* Failed persistence leaves candidates owned by wireless, independent of boot-config RAM rollback. */
    assert(pf_wireless_staged()); pf_wireless_saved(); assert(!pf_wireless_staged());
    ok(7u, 8u, "password"); ok(9u, 0u, NULL);
    snapshot(report); assert(report[1] == 4u && report[46] == 1u);
    assert(joined_length == 32u && memcmp(joined_ssid, raw, 32u) == 0);
    assert(pf_wireless_save_precondition(true) == 25u);
    rejected(6u, 5u, 25u); rejected(11u, 5u, 25u);
    pf_hid_control_request_t bad = {.version = 1u, .transaction_id = 2u, .opcode = 7u};
    bad.arguments[0] = 3u; memcpy(bad.arguments + 1u, "bad", 3u);
    pf_hid_control_result_t result; pf_wireless_command(&bad, &result);
    assert(result.status == 3u && result.result_data[0] == 17u); /* Malformed before Busy. */
    bad.opcode = 9u; bad.arguments[63] = 1u; pf_wireless_command(&bad, &result);
    assert(result.status == 3u && result.error_reason == 1u);
    link = CYW43_LINK_BADAUTH; pf_wireless_poll();
    assert(pf_wireless_staged() && pf_wireless_save_precondition(true) == 0u);
    snapshot(report); assert(report[1] == 6u && report[2] == 3u && (report[4] & 32u) != 0u);
    /* Test failure does not refresh or erase staging. Synchronous status expires without polling. */
    now = STAGING_LIFETIME_US; snapshot(report);
    assert(report[47] == 8u && (report[4] & 0xf0u) == 0u && !pf_wireless_staged());
    assert(pf_wireless_save_precondition(true) == 20u && pf_wireless_save_precondition(false) == 0u);
    rejected(9u, 3u, 20u); pf_wireless_saved(); snapshot(report); assert(report[47] == 0u);
    ok(7u, 8u, "password");
    now += STAGING_LIFETIME_US - 1u; ok(8u, 0u, NULL); /* Any staging refreshes all candidates. */
    ++now; assert(pf_wireless_staged());
    link = CYW43_LINK_DOWN; ok(9u, 0u, NULL);
    now += 1000000u; link = CYW43_LINK_UP; pf_wireless_poll(); snapshot(report);
    assert(report[1] == 5u && (report[4] & 128u) != 0u && report[47] == 4u);
    const uint64_t tested_deadline = now + STAGING_LIFETIME_US;
    now = tested_deadline - 1u; assert(pf_wireless_staged());
    /* Restaging only a key preserves pair validity; restaging either pair field invalidates it. */
    ok(8u, 0u, NULL); snapshot(report); assert((report[4] & 128u) != 0u);
    ok(7u, 8u, "password"); snapshot(report); assert((report[4] & 128u) == 0u);
    assert(pf_wireless_save_precondition(true) == 0u);
    pf_wireless_discard(); snapshot(report); assert(report[47] == 0u && !pf_wireless_staged());

    /* Empty SSID implicitly clears the password, even if a later password value is provided. */
    ok(6u, 0u, NULL); ok(7u, 8u, "password"); snapshot(report);
    assert((report[4] & 48u) == 48u && (report[47] & 3u) == 3u);
    rejected(9u, 3u, 19u);
    assert(pf_wireless_apply_staged(&mask) && mask == 3u && saved_length == 0u && saved_password[0] == 0u);
    pf_wireless_saved();
    /* Same-SSID password updates preserve a proven authentication mode without testing. */
    pf_boot_config_set_wifi_ssid_bytes(raw, 32u); saved_auth = PF_WIFI_AUTH_SAE;
    ok(7u, 8u, "password"); assert(pf_wireless_apply_staged(&mask) && saved_auth == PF_WIFI_AUTH_SAE);
    pf_wireless_discard();
    /* Test generation skips zero and remains independent when a scan is accepted. */
    test_generation = UINT8_MAX;
    ok(7u, 8u, "password"); link = CYW43_LINK_DOWN; ok(9u, 0u, NULL);
    snapshot(report); assert(report[46] == 1u);
    link = CYW43_LINK_UP; pf_wireless_poll(); ok(11u, 0u, NULL);
    snapshot(report); assert(report[46] == 1u && report[42] == 1u && report[1] == 5u);
    assert(pf_wireless_save_precondition(true) == 26u);
    ok(10u, 0u, NULL); assert(driver_scan && !pf_wireless_staged());
    ok(8u, 0u, NULL);
    now += STAGING_LIFETIME_US; snapshot(report);
    assert(report[47] == 8u && driver_scan);
    /* A rejected stage does not clear expiry or mutate candidates. */
    bad = (pf_hid_control_request_t){.version = 1u, .transaction_id = 1u, .opcode = 8u};
    bad.arguments[0] = 1u; pf_wireless_command(&bad, &result);
    snapshot(report); assert(result.status == 3u && report[47] == 8u);
    ok(8u, 0u, NULL); snapshot(report); assert(report[47] == 4u && (report[4] & 64u) != 0u);
    rejected(9u, 5u, 26u); driver_scan = false; pf_wireless_poll();
    rejected(9u, 3u, 21u); /* Key-only staging is not a Wi-Fi pair. */
    pf_wireless_discard();
    ok(7u, 8u, "password");
    link = CYW43_LINK_DOWN; ok(9u, 0u, NULL);
    /* Expiry while joining cancels synchronously, retains generation, and restores the boot link. */
    unsigned before_expiry = leaves;
    now += STAGING_LIFETIME_US;
    assert(pf_wireless_save_precondition(true) == 20u && leaves == before_expiry + 1u);
    snapshot(report); assert(report[47] == 8u && report[46] == 2u);
    link = CYW43_LINK_DOWN; pf_wireless_poll(); assert(pf_wifi_state() == PF_WIFI_STATE_CONNECTING);
    pf_wireless_discard();
    ok(7u, 8u, "password"); link = CYW43_LINK_DOWN; ok(9u, 0u, NULL);
    link = CYW43_LINK_NONET; pf_wireless_poll();
    pf_wireless_poll(); assert(last_auth == CYW43_AUTH_WPA3_SAE_AES_PSK);
    link = CYW43_LINK_UP; pf_wireless_poll(); snapshot(report); assert((report[4] & 128u) != 0u);
    assert(pf_wireless_apply_staged(&mask) && saved_auth == PF_WIFI_AUTH_SAE);
    const unsigned before_saved = leaves;
    pf_wireless_saved(); assert(leaves == before_saved); /* No unnecessary restore before reboot. */
    ok(7u, 8u, "password"); link = CYW43_LINK_DOWN; ok(9u, 0u, NULL);
    link = CYW43_LINK_UP; pf_wireless_poll();
    const unsigned before_joined_expiry = leaves;
    now += STAGING_LIFETIME_US; snapshot(report);
    assert(report[47] == 8u && (report[4] & 0xf0u) == 0u && leaves == before_joined_expiry + 1u);
    link = CYW43_LINK_DOWN; pf_wireless_poll(); assert(link_owner == PF_WIFI_LINK_SAVED);
    assert(zeroizes != 0u);
}

static void service_regressions(void)
{
    reset(); pf_wireless_start(); pf_wifi_start(); link = CYW43_LINK_UP; pf_wireless_poll();
    assert(!serving && auth_required); /* Never fall back to an open console. */
    reset(); memset(saved_key, 0xa5, 32u); saved_key_present = true; pf_wireless_start();
    assert(ble_prepared_with_key); /* BLE is key-gated, so the key must load before its gate is checked. */
    link = CYW43_LINK_UP; pf_wireless_poll(); assert(serving && auth_required);
    pf_hid_control_request_t request = {.version = 1u, .transaction_id = 1u, .opcode = 13u};
    for (unsigned i = 0u; i < 16u; ++i) request.arguments[i] = (uint8_t)i;
    pf_hid_control_result_t result; pf_wireless_command(&request, &result);
    assert(result.status == 2u && memcmp(result.result_data, request.arguments, 16u) == 0);
    assert(bytes_zero(result.result_data + 16u, 47u));
    ok(7u, 8u, "password"); link = CYW43_LINK_DOWN; ok(9u, 0u, NULL);
    link = CYW43_LINK_UP; pf_wireless_poll(); assert(!serving); /* Candidate network is never trusted. */
    pf_wireless_saved(); assert(!pf_wireless_staged());
    pf_boot_config_set_ble_key(NULL); pf_wireless_saved(); pf_wireless_poll(); assert(!serving);
    /* A 32-byte zero key is distinct from length-zero clearing throughout staging, startup and proof admission. */
    reset(); pf_wireless_start();
    uint8_t zero_key[32] = {0}, zero_status[48], mask;
    ok(8u, 32u, zero_key); snapshot(zero_status);
    assert((zero_status[4] & 64u) != 0u && (zero_status[47] & 4u) == 0u);
    assert(pf_wireless_apply_staged(&mask) && mask == 4u && pf_boot_config_ble_key_configured());
    pf_wireless_saved(); pf_wireless_start();
    assert(shared_key);
    link = CYW43_LINK_UP; pf_wireless_poll(); assert(serving && auth_required);
    snapshot(zero_status); assert((zero_status[4] & 2u) != 0u);
    ok(13u, 0u, NULL);
    ok(8u, 0u, NULL); snapshot(zero_status); assert((zero_status[47] & 4u) != 0u);
    assert(pf_wireless_apply_staged(&mask) && mask == 4u && !pf_boot_config_ble_key_configured());
    pf_wireless_saved(); pf_wireless_poll(); assert(!serving);
    rejected(13u, 3u, 24u);
    pf_wireless_start(); assert(!shared_key);
    reset(); init_rc = -1; pf_wireless_start();
    uint8_t report[48]; snapshot(report); assert(report[1] == 3u && report[2] == 1u);
    assert(!pf_wireless_led_put(true));
    ok(7u, 8u, "password"); rejected(9u, 6u, 0u); assert(pf_wireless_staged());
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "wifi") == 0) { radio_regressions(); scan_regressions(); }
    else { staging_regressions(); service_regressions(); }
    return 0;
}
