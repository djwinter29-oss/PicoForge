/**
 * @file mdns.c
 * @brief Bounded mDNS hostname registration on the USB wireless owner task; no resolver or extra SDK checkout.
 */
#include "app/wireless/wifi/mdns.h"

#include <stddef.h>

#include "lwip/apps/mdns.h"
#include "lwip/apps/mdns_out.h"
#include "lwip/timeouts.h"
#include "app/wireless/wifi/tcp_cli.h"
#include "app/wireless/wireless.h"
#include "pico/cyw43_arch.h"
#include "pico/unique_id.h"
#include "platform/logging.h"

#if LWIP_IPV6 || MDNS_MAX_STORED_PKTS != 0
#error "mDNS teardown requires IPv4-only operation without retained truncated queries"
#endif

#define MDNS_RETRY_DELAY_US 5000000u /**< Retry allocation failures without flooding the owner task or log. */

static bool initialized; /**< Global lwIP responder initialized once. */
static bool registered; /**< Saved station netif owns a responder allocation. */
static bool service_registered; /**< TCP CLI DNS-SD service is allocated on the saved station netif. */
static s8_t service_slot; /**< lwIP slot for the TCP CLI service. */
static uint64_t retry_us; /**< Next permitted registration attempt. */

/** @brief Report name conflicts explicitly; no unbounded automatic renaming. */
static void name_result(struct netif *netif, u8_t result, s8_t slot)
{
    (void)netif;
    (void)slot;
    if (result == MDNS_PROBING_CONFLICT) PF_LOGGING_WARNING("mdns", "mDNS hostname conflict");
}

/** @brief Publish the TCP authentication policy with the service record. */
static void service_txt(struct mdns_service *service, void *userdata)
{
    (void)userdata;
    const char *auth = pf_tcp_cli_auth_required() ? "auth=1" : "auth=0";
    if (mdns_resp_add_service_txtitem(service, auth, 6u) != ERR_OK ||
        mdns_resp_add_service_txtitem(service, "v=1", 3u) != ERR_OK) {
        PF_LOGGING_ERROR("mdns", "TCP CLI service TXT record failed");
    }
}

/** @copydoc pf_wifi_mdns_poll */
void pf_wifi_mdns_poll(bool saved_link_up, bool tcp_cli_up)
{
    struct netif *netif = &cyw43_state.netif[CYW43_ITF_STA];
    const uint64_t now = time_us_64();
    if (!saved_link_up) {
        if (registered && now >= retry_us) {
            /* The pinned responder removes only its probe timer; delayed IPv4 callbacks also own netif data. */
            sys_untimeout(mdns_multicast_timeout_reset_ipv4, netif);
            sys_untimeout(mdns_multicast_probe_timeout_reset_ipv4, netif);
            sys_untimeout(mdns_multicast_timeout_25ttl_reset_ipv4, netif);
            sys_untimeout(mdns_send_multicast_msg_delayed_ipv4, netif);
            sys_untimeout(mdns_send_unicast_msg_delayed_ipv4, netif);
            const err_t rc = mdns_resp_remove_netif(netif);
            if (rc != ERR_OK) {
                retry_us = now + MDNS_RETRY_DELAY_US;
                PF_LOGGING_ERROR("mdns", "mDNS removal failed: %d", (int)rc);
            }
            else {
                registered = false;
                service_registered = false;
                retry_us = 0u;
            }
        }
        return;
    }
    if (now < retry_us) return;
    if (!initialized) {
        mdns_resp_init();
        mdns_resp_register_name_result_cb(name_result);
        initialized = true;
    }
    if (!registered) {
        char hostname[sizeof("picoforge-") + 2u * PICO_UNIQUE_BOARD_ID_SIZE_BYTES] = "picoforge-";
        pico_get_unique_board_id_string(hostname + sizeof("picoforge-") - 1u,
                                       sizeof(hostname) - sizeof("picoforge-") + 1u);
        const err_t rc = mdns_resp_add_netif(netif, hostname);
        if (rc != ERR_OK) {
            retry_us = time_us_64() + MDNS_RETRY_DELAY_US;
            PF_LOGGING_ERROR("mdns", "mDNS registration failed: %d", (int)rc);
            return;
        }
        registered = true;
        PF_LOGGING_INFO("mdns", "Hostname: %s.local", hostname);
    }
    if (tcp_cli_up && !service_registered) {
        const s8_t slot = mdns_resp_add_service(netif, "picoforge-cli", "_picoforge", DNSSD_PROTO_TCP,
                                                PF_TCP_CLI_PORT, service_txt, NULL);
        if (slot < 0) {
            retry_us = time_us_64() + MDNS_RETRY_DELAY_US;
            PF_LOGGING_ERROR("mdns", "TCP CLI service registration failed: %d", (int)slot);
            return;
        }
        service_slot = slot;
        service_registered = true;
    } else if (!tcp_cli_up && service_registered) {
        const err_t rc = mdns_resp_del_service(netif, (u8_t)service_slot);
        if (rc != ERR_OK) {
            retry_us = time_us_64() + MDNS_RETRY_DELAY_US;
            PF_LOGGING_ERROR("mdns", "TCP CLI service removal failed: %d", (int)rc);
            return;
        }
        service_registered = false;
    }
}
