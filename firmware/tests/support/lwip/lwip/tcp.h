/**
 * @file tcp.h
 * @brief Minimal host fake of the lwIP raw TCP API used by app/wireless/wifi/tcp_cli.c.
 */
#ifndef PF_TEST_FAKE_LWIP_TCP_H
#define PF_TEST_FAKE_LWIP_TCP_H

#include <stdbool.h>
#include <stdint.h>

typedef uint8_t u8_t;
typedef uint16_t u16_t;
typedef uint32_t u32_t;
typedef int8_t err_t;

#define ERR_OK 0
#define ERR_MEM (-1)
#define ERR_VAL (-6)
#define ERR_ABRT (-13)
#define ERR_RST (-14)

#define TCP_WND 64u /**< Small window so tests can fill the receive ring. */
#define TCP_SND_QUEUELEN 4u
#define SOF_KEEPALIVE 0x08u
#define IPADDR_TYPE_V4 0u
#define IP_ANY_TYPE NULL
#define TCP_WRITE_FLAG_COPY 0x01u

/** Fake PCB: the lwIP fields tcp_cli.c touches plus recorded fake state. */
struct tcp_pcb {
    u8_t so_options; /**< Socket options set by the code under test. */
    u32_t keep_idle; /**< Keepalive idle time. */
    u32_t keep_intvl; /**< Keepalive probe interval. */
    u32_t keep_cnt; /**< Keepalive probe count. */
    u16_t snd_buf; /**< Free send space reported by tcp_sndbuf(). */
    u16_t snd_queuelen; /**< Queued segments reported by tcp_sndqueuelen(). */
    bool nodelay; /**< Nagle disabled. */
    bool closed; /**< tcp_close() succeeded. */
    bool aborted; /**< tcp_abort() was called. */
    char sent[256]; /**< Bytes accepted by tcp_write(). */
    u16_t sent_size; /**< Valid bytes in sent. */
    u32_t recved; /**< Window credit returned through tcp_recved(). */
};

/** Fake single-segment pbuf. */
struct pbuf {
    u16_t tot_len; /**< Payload length. */
    const void *payload; /**< Payload bytes. */
};

typedef err_t (*tcp_accept_fn)(void *arg, struct tcp_pcb *newpcb, err_t err);
typedef err_t (*tcp_recv_fn)(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
typedef void (*tcp_err_fn)(void *arg, err_t err);

#define tcp_sndbuf(pcb) ((pcb)->snd_buf)
#define tcp_sndqueuelen(pcb) ((pcb)->snd_queuelen)
#define tcp_nagle_disable(pcb) ((pcb)->nodelay = true)

struct tcp_pcb *tcp_new_ip_type(u8_t type);
err_t tcp_bind(struct tcp_pcb *pcb, const void *ipaddr, u16_t port);
struct tcp_pcb *tcp_listen_with_backlog(struct tcp_pcb *pcb, u8_t backlog);
void tcp_arg(struct tcp_pcb *pcb, void *arg);
void tcp_accept(struct tcp_pcb *pcb, tcp_accept_fn accept);
void tcp_recv(struct tcp_pcb *pcb, tcp_recv_fn recv);
void tcp_err(struct tcp_pcb *pcb, tcp_err_fn err);
err_t tcp_write(struct tcp_pcb *pcb, const void *data, u16_t len, u8_t apiflags);
err_t tcp_output(struct tcp_pcb *pcb);
void tcp_recved(struct tcp_pcb *pcb, u16_t len);
err_t tcp_close(struct tcp_pcb *pcb);
void tcp_abort(struct tcp_pcb *pcb);
u16_t pbuf_copy_partial(const struct pbuf *p, void *dataptr, u16_t len, u16_t offset);
u8_t pbuf_free(struct pbuf *p);

#endif
