/**
 * @file wifi.c
 * @brief Asynchronous WPA2 station connection, DHCP diagnostics and paced reconnects.
 */
#include "app/wireless/wifi.h"

#include <string.h>

#include "pico/cyw43_arch.h"
#include "platform/boot_config.h"
#include "platform/logging.h"

static char wifi_ssid[33]; /**< Boot-applied SSID. */
static char wifi_password[64]; /**< Boot-applied private passphrase. */
static uint64_t connect_deadline_us; /**< Wi-Fi association/DHCP attempt deadline. */
static uint64_t retry_us; /**< Next permitted Wi-Fi reconnect attempt. */
static uint8_t wifi_state; /**< 0 disabled, 1 connecting, 2 connected, 3 failed. */

/** @brief Begin an asynchronous WPA2 connection; retries are paced after explicit failures. */
static void connect_wifi(uint8_t *error)
{
    int rc = cyw43_arch_wifi_connect_async(wifi_ssid, wifi_password, CYW43_AUTH_WPA2_AES_PSK);
    wifi_state = rc == 0 ? 1u : 3u;
    *error = rc == 0 ? 0u : 2u;
    connect_deadline_us = time_us_64() + 30000000u;
    retry_us = time_us_64() + 5000000u;
    if (rc != 0) PF_LOGGING_ERROR("wifi", "Wi-Fi connection request failed: %d", rc);
}

/** @copydoc pf_wifi_prepare */
bool pf_wifi_prepare(void)
{
    pf_boot_config_wifi_ssid(wifi_ssid);
    pf_boot_config_wifi_password(wifi_password);
    return (pf_boot_config_radios() & PF_WIRELESS_WIFI) != 0u &&
        wifi_ssid[0] != '\0' && wifi_password[0] != '\0';
}

/** @copydoc pf_wifi_start */
void pf_wifi_start(uint8_t *error)
{
    cyw43_arch_enable_sta_mode();
    connect_wifi(error);
}

/** @copydoc pf_wifi_poll */
void pf_wifi_poll(uint8_t *error)
{
    int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    uint64_t now = time_us_64();
    if (link == CYW43_LINK_UP) {
        wifi_state = 2u;
        *error = 0u;
    } else if (wifi_state == 1u && (link < 0 || now >= connect_deadline_us)) {
        wifi_state = 3u;
        *error = link < 0 ? 2u : 3u;
        retry_us = now + 5000000u;
        PF_LOGGING_WARNING("wifi", "Wi-Fi association/DHCP failed: %d", link);
    } else if (wifi_state == 2u) {
        wifi_state = 3u;
        retry_us = now + 5000000u;
        PF_LOGGING_WARNING("wifi", "Wi-Fi link lost");
    } else if (wifi_state == 3u && now >= retry_us) connect_wifi(error);
}

/** @copydoc pf_wifi_state */
uint8_t pf_wifi_state(void)
{
    return wifi_state;
}

/** @copydoc pf_wifi_ip_address */
void pf_wifi_ip_address(uint8_t address[4])
{
    memcpy(address, &netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA])->addr, 4u);
}
