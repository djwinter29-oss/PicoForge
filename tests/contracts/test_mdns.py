"""Exercise saved-network mDNS registration against the SDK API without a radio."""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.contracts


def test_mdns_has_bounded_timeout_capacity():
    options = (ROOT / "firmware/src/integration/wireless/lwipopts.h").read_text()
    assert "#define LWIP_MDNS_SEARCH 0" in options
    assert "#define MDNS_MAX_STORED_PKTS 0" in options
    assert "#define MEMP_NUM_SYS_TIMEOUT (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 6)" in options


def test_mdns_saved_network_lifecycle_retry_and_hostname(tmp_path):
    headers = {
        "pico/cyw43_arch.h": """
#include <stdint.h>
#define CYW43_ITF_STA 0
struct netif { int unused; };
typedef struct { struct netif netif[1]; } cyw43_t;
extern cyw43_t cyw43_state;
uint64_t time_us_64(void);
""",
        "pico/unique_id.h": """
#define PICO_UNIQUE_BOARD_ID_SIZE_BYTES 8
void pico_get_unique_board_id_string(char *, unsigned);
""",
        "lwip/apps/mdns.h": """
#include <stdint.h>
typedef uint8_t u8_t;
typedef int8_t s8_t;
typedef int err_t;
struct netif;
struct mdns_service;
typedef void (*service_get_txt_fn_t)(struct mdns_service *, void *);
#define ERR_OK 0
#define MDNS_PROBING_CONFLICT 0
void mdns_resp_init(void);
void mdns_resp_register_name_result_cb(void (*)(struct netif *, u8_t, s8_t));
err_t mdns_resp_add_netif(struct netif *, const char *);
err_t mdns_resp_remove_netif(struct netif *);
s8_t mdns_resp_add_service(struct netif *, const char *, const char *, int, uint16_t, service_get_txt_fn_t, void *);
err_t mdns_resp_del_service(struct netif *, uint8_t);
err_t mdns_resp_add_service_txtitem(struct mdns_service *, const char *, uint8_t);
#define DNSSD_PROTO_TCP 0
""",
        "lwip/apps/mdns_out.h": """
void mdns_multicast_timeout_reset_ipv4(void *);
void mdns_multicast_probe_timeout_reset_ipv4(void *);
void mdns_multicast_timeout_25ttl_reset_ipv4(void *);
void mdns_send_multicast_msg_delayed_ipv4(void *);
void mdns_send_unicast_msg_delayed_ipv4(void *);
""",
        "lwip/timeouts.h": "void sys_untimeout(void (*)(void *), void *);\n",
    }
    for name, text in headers.items():
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    harness = tmp_path / "harness.c"
    harness.write_text(r"""
#include <assert.h>
#include <string.h>
#include "app/wireless/wifi/mdns.h"
#include "lwip/apps/mdns.h"
#include "pico/cyw43_arch.h"
#include "platform/logging.h"
cyw43_t cyw43_state;
struct mdns_service { int unused; };
static unsigned inits, adds, removes, logs, canceled, service_adds, service_removes, txt_items;
static uint64_t now;
static int add_result, service_result, remove_result;
static bool auth_required = true;
static void (*name_cb)(struct netif *, u8_t, s8_t);
bool pf_tcp_cli_auth_required(void) { return auth_required; }
uint64_t time_us_64(void) { return now; }
void pico_get_unique_board_id_string(char *out, unsigned length)
{ assert(length == 17u); memcpy(out, "0123456789ABCDEF", 17u); }
void mdns_resp_init(void) { ++inits; }
void mdns_resp_register_name_result_cb(void (*cb)(struct netif *, u8_t, s8_t)) { name_cb = cb; }
err_t mdns_resp_add_netif(struct netif *netif, const char *name)
{
    assert(netif == &cyw43_state.netif[0]);
    assert(strcmp(name, "picoforge-0123456789ABCDEF") == 0);
    ++adds;
    return add_result;
}
err_t mdns_resp_remove_netif(struct netif *netif)
{ assert(netif == &cyw43_state.netif[0] && canceled == 5u); canceled = 0u; ++removes; return remove_result; }
s8_t mdns_resp_add_service(struct netif *netif, const char *name, const char *service, int proto,
                           uint16_t port, service_get_txt_fn_t txt_fn, void *userdata)
{
    assert(netif == &cyw43_state.netif[0] && strcmp(name, "picoforge-cli") == 0);
    assert(strcmp(service, "_picoforge") == 0 && proto == DNSSD_PROTO_TCP && port == 2323u);
    assert(txt_fn != NULL && userdata == NULL);
    ++service_adds;
    struct mdns_service service_record;
    txt_fn(&service_record, userdata);
    return (s8_t)service_result;
}
err_t mdns_resp_add_service_txtitem(struct mdns_service *service, const char *txt, uint8_t length)
{
    assert(service != NULL);
    assert((length == 6u && strcmp(txt, auth_required ? "auth=1" : "auth=0") == 0) ||
           (length == 3u && strcmp(txt, "v=1") == 0));
    ++txt_items;
    return ERR_OK;
}
err_t mdns_resp_del_service(struct netif *netif, uint8_t slot)
{ assert(netif == &cyw43_state.netif[0] && slot == 0u); ++service_removes; return ERR_OK; }
void mdns_multicast_timeout_reset_ipv4(void *arg) { (void)arg; }
void mdns_multicast_probe_timeout_reset_ipv4(void *arg) { (void)arg; }
void mdns_multicast_timeout_25ttl_reset_ipv4(void *arg) { (void)arg; }
void mdns_send_multicast_msg_delayed_ipv4(void *arg) { (void)arg; }
void mdns_send_unicast_msg_delayed_ipv4(void *arg) { (void)arg; }
void sys_untimeout(void (*handler)(void *), void *arg)
{ assert(handler != NULL && arg == &cyw43_state.netif[0]); ++canceled; }
bool pf_logging_write(pf_logging_level_t level, const char *tag, const char *format, ...)
{ (void)level; (void)format; assert(strcmp(tag, "mdns") == 0); ++logs; return true; }
int main(void)
{
    pf_wifi_mdns_poll(false, false);
    assert(inits == 0 && adds == 0 && removes == 0);
    add_result = -1;
    pf_wifi_mdns_poll(true, false);
    assert(inits == 1 && adds == 1 && logs == 1);
    now = 4999999u;
    pf_wifi_mdns_poll(true, false);
    assert(adds == 1);
    now = 5000000u;
    add_result = 0;
    pf_wifi_mdns_poll(true, true);
    assert(inits == 1 && adds == 2);
    assert(service_adds == 1);
    pf_wifi_mdns_poll(true, true);
    assert(adds == 2 && service_adds == 1);
    pf_wifi_mdns_poll(true, false);
    assert(service_removes == 1);
    name_cb(&cyw43_state.netif[0], MDNS_PROBING_CONFLICT, -1);
    assert(logs == 3 && txt_items == 2);
    remove_result = -1;
    pf_wifi_mdns_poll(false, false);
    pf_wifi_mdns_poll(false, false);
    assert(removes == 1 && logs == 4);
    now += 5000000u;
    remove_result = ERR_OK;
    pf_wifi_mdns_poll(false, false);
    assert(removes == 2);
    now += 5000000u;
    pf_wifi_mdns_poll(false, false);
    assert(removes == 2);
    pf_wifi_mdns_poll(true, true);
    assert(inits == 1 && adds == 3);
    assert(service_adds == 2);
    pf_wifi_mdns_poll(false, false);
    assert(removes == 3);
    return 0;
}
""")
    binary = tmp_path / "mdns"
    compiled = subprocess.run([
        "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(tmp_path), "-I", str(ROOT / "firmware/src"),
        str(harness), str(ROOT / "firmware/src/app/wireless/wifi/mdns.c"), "-o", str(binary),
    ], check=False, capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stderr
    subprocess.run([str(binary)], check=True)
