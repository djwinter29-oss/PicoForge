/**
 * @file mdns.h
 * @brief SDK mDNS hostname ownership for the saved station network.
 */
#ifndef PF_WIFI_MDNS_H
#define PF_WIFI_MDNS_H

#include <stdbool.h>

/**
 * @brief Advertise picoforge-<board ID>.local only on the saved network, with paced allocation retries.
 * @param saved_link_up False removes the responder before a test or station shutdown.
 * @param tcp_cli_up True advertises the TCP CLI service on the active saved link.
 */
void pf_wifi_mdns_poll(bool saved_link_up, bool tcp_cli_up);

#endif
