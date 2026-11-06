// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

#ifndef PF_LWIPOPTS_H
#define PF_LWIPOPTS_H

#define NO_SYS 1
/* lwIP defaults to byte alignment; Cortex-M multi-word stores into unaligned pbuf headers fault on received frames. */
#define MEM_ALIGNMENT 4
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#define LWIP_TCP 1
/* TCP serves only the single-client raw CLI (app/wireless/wifi/tcp_cli.c); keep its buffers bounded. */
#define TCP_MSS 1460
#define TCP_WND (2 * TCP_MSS)
#define TCP_SND_BUF (2 * TCP_MSS)
#define MEMP_NUM_TCP_PCB 3
#define MEMP_NUM_TCP_PCB_LISTEN 1
#define LWIP_TCP_KEEPALIVE 1
#define LWIP_UDP 1
#define LWIP_DHCP 1
#define LWIP_DNS 0
#define LWIP_IGMP 1
#define LWIP_MDNS_RESPONDER 1
#define LWIP_MDNS_SEARCH 0
#define LWIP_NUM_NETIF_CLIENT_DATA 1
#define LWIP_NETIF_EXT_STATUS_CALLBACK 1
#define MDNS_MAX_SERVICES 1
/* ponytail: hostname-only discovery does not retain truncated queries. The pinned responder cannot cancel their
 * private callbacks during netif removal. Enable this pool after upgrading to a responder with complete teardown. */
#define MDNS_MAX_STORED_PKTS 0
/* One interface: probe plus five delayed IPv4 response/holdoff timers. */
#define MEMP_NUM_SYS_TIMEOUT (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 6)
#define LWIP_IPV6 0
#define LWIP_NETIF_HOSTNAME 1
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define MEM_SIZE 8192
#define MEMP_NUM_PBUF 8
#define PBUF_POOL_SIZE 8
#define PBUF_POOL_BUFSIZE 1536
#define LWIP_STATS 0
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1

#endif
