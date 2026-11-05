#ifndef PF_TEST_CYW43_ARCH_H
#define PF_TEST_CYW43_ARCH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define CYW43_AUTH_OPEN 0u
#define CYW43_AUTH_WPA2_MIXED_PSK 0x00400006u
#define CYW43_AUTH_WPA3_SAE_AES_PSK 0x01000004u
#define CYW43_CHANNEL_NONE 0xffffffffu
#define CYW43_ITF_STA 0
#define CYW43_WL_GPIO_LED_PIN 0
#define CYW43_LINK_DOWN 0
#define CYW43_LINK_JOIN 1
#define CYW43_LINK_NOIP 2
#define CYW43_LINK_UP 3
#define CYW43_LINK_NONET (-2)
#define CYW43_LINK_BADAUTH (-3)
typedef struct { uint8_t unused; } cyw43_wifi_scan_options_t;
typedef struct {
    uint8_t ssid_len;
    uint8_t ssid[32];
    int16_t rssi;
    uint8_t auth_mode;
} cyw43_ev_scan_result_t;
typedef struct { uint32_t addr; } ip4_addr_t;
typedef struct { ip4_addr_t ip; } netif_t;
typedef struct { netif_t netif[1]; uint32_t wifi_join_state; } cyw43_t;
typedef int (*scan_result_cb_t)(void *, const cyw43_ev_scan_result_t *);
extern cyw43_t cyw43_state;
#define netif_ip4_addr(netif) (&(netif)->ip)
bool cyw43_wifi_scan_active(cyw43_t *);
int cyw43_wifi_scan(cyw43_t *, cyw43_wifi_scan_options_t *, void *, scan_result_cb_t);
int cyw43_wifi_leave(cyw43_t *, int);
int cyw43_wifi_join(cyw43_t *, size_t, const uint8_t *, size_t, const uint8_t *, uint32_t,
                    const uint8_t *, uint32_t);
int cyw43_tcpip_link_status(cyw43_t *, int);
int cyw43_wifi_get_rssi(cyw43_t *, int32_t *);
uint64_t time_us_64(void);
void cyw43_arch_enable_sta_mode(void);
void cyw43_arch_disable_sta_mode(void);
int cyw43_arch_init(void);
void cyw43_arch_poll(void);
unsigned get_core_num(void);
void cyw43_arch_gpio_put(unsigned, bool);
#endif
