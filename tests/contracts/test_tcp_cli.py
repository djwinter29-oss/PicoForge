"""Exercise the production raw TCP CLI transport against a minimal fake lwIP raw API."""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.contracts

FAKE_LWIP = r"""
#ifndef FAKE_LWIP_TCP_H
#define FAKE_LWIP_TCP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int8_t err_t;
typedef uint8_t u8_t;
typedef uint16_t u16_t;
typedef uint32_t u32_t;
#define ERR_OK 0
#define ERR_MEM (-1)
#define ERR_VAL (-6)
#define ERR_ABRT (-13)
#define TCP_WND 64
#define TCP_SND_QUEUELEN 4
#define TCP_WRITE_FLAG_COPY 1
#define IPADDR_TYPE_V4 0
#define IP_ANY_TYPE NULL
#define SOF_KEEPALIVE 0x08u
struct pbuf { u16_t tot_len; const uint8_t *payload; };
struct tcp_pcb;
typedef err_t (*tcp_recv_fn)(void *, struct tcp_pcb *, struct pbuf *, err_t);
typedef void (*tcp_err_fn)(void *, err_t);
typedef err_t (*tcp_accept_fn)(void *, struct tcp_pcb *, err_t);
struct tcp_pcb {
    const char *remote_ip;
    u8_t so_options;
    u32_t keep_idle, keep_intvl, keep_cnt;
    u16_t snd_buf, snd_queuelen, recved;
    bool nagle_off, closed, aborted;
    char written[128];
    size_t written_length;
    tcp_recv_fn recv;
    tcp_err_fn err;
    tcp_accept_fn accept;
};
#define ipaddr_ntoa(address) (*(address) ? *(address) : "0.0.0.0")
#define tcp_sndbuf(pcb) ((pcb)->snd_buf)
#define tcp_sndqueuelen(pcb) ((pcb)->snd_queuelen)
#define tcp_nagle_disable(pcb) ((pcb)->nagle_off = true)
struct tcp_pcb *tcp_new_ip_type(u8_t type);
err_t tcp_bind(struct tcp_pcb *pcb, const void *address, u16_t port);
struct tcp_pcb *tcp_listen_with_backlog(struct tcp_pcb *pcb, u8_t backlog);
void tcp_accept(struct tcp_pcb *pcb, tcp_accept_fn accept);
void tcp_arg(struct tcp_pcb *pcb, void *argument);
void tcp_recv(struct tcp_pcb *pcb, tcp_recv_fn recv);
void tcp_err(struct tcp_pcb *pcb, tcp_err_fn err);
err_t tcp_close(struct tcp_pcb *pcb);
void tcp_abort(struct tcp_pcb *pcb);
err_t tcp_write(struct tcp_pcb *pcb, const void *data, u16_t length, u8_t flags);
err_t tcp_output(struct tcp_pcb *pcb);
void tcp_recved(struct tcp_pcb *pcb, u16_t length);
u16_t pbuf_copy_partial(const struct pbuf *buffer, void *data, u16_t length, u16_t offset);
u8_t pbuf_free(struct pbuf *buffer);
#endif
"""

HARNESS = r"""
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "lwip/tcp.h"
#include "app/wireless/wifi/tcp_cli.h"
#include "app/wireless/wireless_auth.h"
#include "platform/logging.h"

static struct tcp_pcb pcbs[20];
static struct tcp_pcb listeners[4];
static unsigned allocated, listener_count, frees, bound_port;
static err_t close_result = ERR_OK;
static uint64_t fake_time;
static bool key_available;
static char last_log[160];
static bool timeout_logged;

struct tcp_pcb *tcp_new_ip_type(u8_t type)
{ assert(type == IPADDR_TYPE_V4); ++allocated; return &listeners[listener_count++]; }
err_t tcp_bind(struct tcp_pcb *pcb, const void *address, u16_t port)
{ (void)pcb; assert(address == NULL); bound_port = port; return ERR_OK; }
struct tcp_pcb *tcp_listen_with_backlog(struct tcp_pcb *pcb, u8_t backlog) { assert(backlog == 1u); return pcb; }
void tcp_accept(struct tcp_pcb *pcb, tcp_accept_fn accept) { pcb->accept = accept; }
void tcp_arg(struct tcp_pcb *pcb, void *argument) { (void)pcb; assert(argument == NULL); }
void tcp_recv(struct tcp_pcb *pcb, tcp_recv_fn recv) { pcb->recv = recv; }
void tcp_err(struct tcp_pcb *pcb, tcp_err_fn err) { pcb->err = err; }
err_t tcp_close(struct tcp_pcb *pcb) { if (close_result == ERR_OK) pcb->closed = true; return close_result; }
void tcp_abort(struct tcp_pcb *pcb) { pcb->aborted = true; }
err_t tcp_write(struct tcp_pcb *pcb, const void *data, u16_t length, u8_t flags)
{
    assert(flags == TCP_WRITE_FLAG_COPY && length <= pcb->snd_buf);
    memcpy(pcb->written + pcb->written_length, data, length);
    pcb->written_length += length;
    pcb->snd_buf = (u16_t)(pcb->snd_buf - length);
    return ERR_OK;
}
err_t tcp_output(struct tcp_pcb *pcb) { (void)pcb; return ERR_OK; }
void tcp_recved(struct tcp_pcb *pcb, u16_t length) { pcb->recved = (u16_t)(pcb->recved + length); }
u16_t pbuf_copy_partial(const struct pbuf *buffer, void *data, u16_t length, u16_t offset)
{ memcpy(data, buffer->payload + offset, length); return length; }
u8_t pbuf_free(struct pbuf *buffer) { (void)buffer; ++frees; return 1u; }
bool pf_logging_write(pf_logging_level_t level, const char *tag, const char *format, ...)
{
    (void)level;
    assert(strcmp(tag, "tcp-cli") == 0);
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(last_log, sizeof(last_log), format, arguments);
    va_end(arguments);
    if (strcmp(last_log, "Client 192.0.2.13 authentication timed out") == 0) timeout_logged = true;
    return true;
}
uint64_t time_us_64(void) { return fake_time; }
void pf_wireless_auth_set_key(const uint8_t key[32]) { key_available = key != NULL && key[0] == 0x42u; }
bool pf_wireless_auth_available(void) { return key_available; }
bool pf_wireless_auth_new_nonce(uint8_t nonce[16])
{
    for (unsigned i = 0u; i < 16u; ++i) nonce[i] = (uint8_t)i;
    return true;
}
void pf_wireless_auth_device_id(uint8_t uid[8])
{ for (unsigned i = 0u; i < 8u; ++i) uid[i] = (uint8_t)i; }
bool pf_wireless_auth_verify(const uint8_t nonce[16], const uint8_t domain[9], const uint8_t request[9],
                             const uint8_t proof[16])
{
    return key_available && memcmp(domain, "PFTCPCLI1", 9u) == 0 &&
        memcmp(request, "TCPCLI001", 9u) == 0 && memcmp(nonce, proof, 16u) == 0;
}

static err_t deliver(struct tcp_pcb *pcb, const char *text, uint16_t length)
{
    struct pbuf buffer = {length, (const uint8_t *)text};
    return pcb->recv(NULL, pcb, &buffer, ERR_OK);
}

int main(void)
{
    char c;
    pf_tcp_cli_poll(true);
    pf_tcp_cli_poll(true);
    assert(allocated == 1u && bound_port == PF_TCP_CLI_PORT);
    assert(pf_tcp_cli_serving());
    struct tcp_pcb *listener = &listeners[0], *client = &pcbs[1], *extra = &pcbs[2];
    assert(!pf_tcp_cli_connected() && !pf_tcp_cli_read(&c) && pf_tcp_cli_write_available() == 0u);

    client->snd_buf = 10u;
    client->remote_ip = "192.0.2.1";
    assert(listener->accept(NULL, client, ERR_OK) == ERR_OK && pf_tcp_cli_connected());
    assert(strcmp(last_log, "Client 192.0.2.1 connected") == 0);
    assert(client->nagle_off && (client->so_options & SOF_KEEPALIVE) != 0u && client->keep_cnt == 4u);

    /* A second client is told the CLI is busy and closed; the first keeps the session. */
    extra->snd_buf = 100u;
    assert(listener->accept(NULL, extra, ERR_OK) == ERR_OK);
    assert(extra->closed && strstr(extra->written, "busy") != NULL && pf_tcp_cli_connected());
    extra->snd_buf = 0u;
    close_result = ERR_VAL;
    assert(listener->accept(NULL, extra, ERR_OK) == ERR_ABRT && extra->aborted);
    close_result = ERR_OK;

    /* Input wraps around the receive ring and reopens the window only after it is read. */
    char first[40], second[50];
    for (unsigned i = 0u; i < sizeof(first); ++i) first[i] = (char)('A' + i % 26u);
    for (unsigned i = 0u; i < sizeof(second); ++i) second[i] = (char)('a' + i % 26u);
    assert(deliver(client, first, sizeof(first)) == ERR_OK);
    for (unsigned i = 0u; i < 30u; ++i) assert(pf_tcp_cli_read(&c) && c == first[i]);
    assert(client->recved == 0u);
    pf_tcp_cli_flush();
    assert(client->recved == 30u);
    assert(deliver(client, second, sizeof(second)) == ERR_OK);
    assert(deliver(client, "x", 5u) == ERR_MEM);
    for (unsigned i = 30u; i < sizeof(first); ++i) assert(pf_tcp_cli_read(&c) && c == first[i]);
    for (unsigned i = 0u; i < sizeof(second); ++i) assert(pf_tcp_cli_read(&c) && c == second[i]);
    assert(!pf_tcp_cli_read(&c));
    pf_tcp_cli_flush();
    assert(client->recved == 90u);

    /* Output is bounded by the send buffer and the send queue. */
    assert(pf_tcp_cli_write("hello world", 11u) == 10u && memcmp(client->written, "hello worl", 10u) == 0);
    assert(pf_tcp_cli_write("d", 1u) == 0u);
    client->snd_buf = 10u;
    client->snd_queuelen = TCP_SND_QUEUELEN;
    assert(pf_tcp_cli_write_available() == 0u);
    client->snd_queuelen = 0u;

    /* lwIP has freed the PCB on error; the transport only forgets it and its unread input. */
    assert(deliver(client, "abc", 3u) == ERR_OK);
    assert(pf_tcp_cli_read(&c) && c == 'a' && pf_tcp_cli_read(&c) && c == 'b');
    client->err(NULL, ERR_ABRT);
    assert(!pf_tcp_cli_connected() && !pf_tcp_cli_read(&c));

    /* Peer FIN closes the session; data for a stale PCB is dropped. */
    struct tcp_pcb *next = &pcbs[3];
    next->snd_buf = 100u;
    assert(listener->accept(NULL, next, ERR_OK) == ERR_OK && pf_tcp_cli_connected());
    unsigned frees_before = frees;
    assert(deliver(client, "stale", 5u) == ERR_OK && frees == frees_before + 1u);
    assert(next->recv(NULL, next, NULL, ERR_OK) == ERR_OK && next->closed && !pf_tcp_cli_connected());

    /* A local close sends the notice before closing. */
    struct tcp_pcb *last = &pcbs[4];
    last->snd_buf = 100u;
    assert(listener->accept(NULL, last, ERR_OK) == ERR_OK);
    pf_tcp_cli_close("bye\r\n");
    assert(last->closed && strcmp(last->written, "bye\r\n") == 0 && !pf_tcp_cli_connected());

    /* Leaving the saved network closes the client and listener, freeing the TCP PCB slot. */
    struct tcp_pcb *served = &pcbs[5];
    served->snd_buf = 100u;
    assert(pf_tcp_cli_serving());
    assert(listener->accept(NULL, served, ERR_OK) == ERR_OK && pf_tcp_cli_connected());
    pf_tcp_cli_poll(false);
    assert(served->closed && !pf_tcp_cli_connected());
    assert(!pf_tcp_cli_serving());
    assert(listener->closed && listener->accept == NULL);
    pf_tcp_cli_poll(true);
    listener = &listeners[1];
    struct tcp_pcb *restored = &pcbs[9];
    restored->snd_buf = 100u;
    assert(pf_tcp_cli_serving());
    assert(listener->accept(NULL, restored, ERR_OK) == ERR_OK && pf_tcp_cli_connected() && !restored->closed);
    pf_tcp_cli_poll(false);

    /* Authenticated mode sends one nonce+UID challenge, withholds CLI I/O, then admits only the HMAC proof. */
    uint8_t key[32] = {0x42u};
    pf_wireless_auth_set_key(key);
    assert(pf_tcp_cli_set_auth_required(true));
    pf_tcp_cli_poll(true);
    listener = &listeners[2];
    struct tcp_pcb *auth_client = &pcbs[11];
    auth_client->snd_buf = 128u;
    assert(listener->accept(NULL, auth_client, ERR_OK) == ERR_OK);
    assert(!pf_tcp_cli_connected() && strstr(auth_client->written, "PicoForge AUTH ") == auth_client->written);
    char auth_line[40] = "AUTH ";
    memcpy(auth_line + 5u, auth_client->written + 15u, 32u);
    memcpy(auth_line + 37u, "\r\n", 2u);
    assert(deliver(auth_client, auth_line, 20u) == ERR_OK && !pf_tcp_cli_connected());
    char oversized[TCP_WND + 1u];
    memcpy(oversized, auth_line + 20u, 19u);
    memset(oversized + 19u, 'x', sizeof(oversized) - 19u);
    const unsigned before_refusal_frees = frees;
    assert(deliver(auth_client, oversized, sizeof(oversized)) == ERR_MEM);
    assert(auth_client->recved == 20u && frees == before_refusal_frees && !pf_tcp_cli_connected());
    assert(deliver(auth_client, auth_line + 20u, 19u) == ERR_OK && pf_tcp_cli_connected());
    assert(auth_client->recved == 39u);
    assert(pf_tcp_cli_write_available() != 0u);
    pf_tcp_cli_poll(false);

    assert(pf_tcp_cli_set_auth_required(true));
    pf_tcp_cli_poll(true);
    listener = &listeners[3];
    struct tcp_pcb *expired = &pcbs[13];
    expired->snd_buf = 128u;
    expired->remote_ip = "192.0.2.13";
    assert(listener->accept(NULL, expired, ERR_OK) == ERR_OK);
    fake_time += 10000000u;
    pf_tcp_cli_poll(true);
    assert(expired->closed && !pf_tcp_cli_connected());
    assert(timeout_logged);
    struct tcp_pcb *rejected = &pcbs[14];
    rejected->snd_buf = 128u;
    rejected->remote_ip = "192.0.2.14";
    assert(listener->accept(NULL, rejected, ERR_OK) == ERR_OK);
    assert(deliver(rejected, "AUTH 00000000000000000000000000000000\r\n", 39u) == ERR_OK);
    assert(rejected->closed);
    assert(strcmp(last_log, "Client 192.0.2.14 authentication failed") == 0);
    struct tcp_pcb *rate_limited = &pcbs[15];
    rate_limited->snd_buf = 128u;
    assert(listener->accept(NULL, rate_limited, ERR_OK) == ERR_OK && rate_limited->closed);
    fake_time += 2000000u;
    struct tcp_pcb *idle = &pcbs[16];
    idle->snd_buf = 128u;
    assert(listener->accept(NULL, idle, ERR_OK) == ERR_OK);
    char valid_proof[40] = "AUTH ";
    memcpy(valid_proof + 5u, idle->written + 15u, 32u);
    memcpy(valid_proof + 37u, "\r\n", 2u);
    assert(deliver(idle, valid_proof, sizeof(valid_proof)) == ERR_OK && pf_tcp_cli_connected());
    fake_time += 900000000u;
    pf_tcp_cli_poll(true);
    assert(idle->closed);
    return 0;
}
"""


def test_tcp_cli_single_session_window_and_backpressure(tmp_path):
    (tmp_path / "lwip").mkdir()
    (tmp_path / "lwip/tcp.h").write_text(FAKE_LWIP)
    for name, text in {
        "pico/time.h": "#include <stdint.h>\nuint64_t time_us_64(void);\n",
    }.items():
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    harness = tmp_path / "harness.c"
    harness.write_text(HARNESS)
    binary = tmp_path / "tcp_cli"
    compiled = subprocess.run([
        "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(tmp_path), "-I", str(ROOT / "firmware/src"),
        str(harness), str(ROOT / "firmware/src/app/wireless/wifi/tcp_cli.c"), "-o", str(binary),
    ], check=False, capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stderr
    subprocess.run([str(binary)], check=True)
