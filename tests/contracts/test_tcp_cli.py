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
#include <string.h>
#include "lwip/tcp.h"
#include "app/wireless/wifi/tcp_cli.h"
#include "platform/logging.h"

static struct tcp_pcb pcbs[8];
static unsigned allocated, frees, bound_port;
static err_t close_result = ERR_OK;

struct tcp_pcb *tcp_new_ip_type(u8_t type) { assert(type == IPADDR_TYPE_V4); return &pcbs[allocated++]; }
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
{ (void)level; (void)format; assert(strcmp(tag, "tcp-cli") == 0); return true; }

static err_t deliver(struct tcp_pcb *pcb, const char *text, uint16_t length)
{
    struct pbuf buffer = {length, (const uint8_t *)text};
    return pcb->recv(NULL, pcb, &buffer, ERR_OK);
}

int main(void)
{
    char c;
    assert(pf_tcp_cli_start() && pf_tcp_cli_start() && allocated == 1u && bound_port == PF_TCP_CLI_PORT);
    struct tcp_pcb *listener = &pcbs[0], *client = &pcbs[1], *extra = &pcbs[2];
    assert(!pf_tcp_cli_connected() && !pf_tcp_cli_read(&c) && pf_tcp_cli_write_available() == 0u);

    client->snd_buf = 10u;
    assert(listener->accept(NULL, client, ERR_OK) == ERR_OK && pf_tcp_cli_connected());
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

    /* Leaving the saved network closes the client with a notice and turns new clients away until it is back. */
    struct tcp_pcb *served = &pcbs[5], *refused = &pcbs[6], *restored = &pcbs[7];
    served->snd_buf = refused->snd_buf = restored->snd_buf = 100u;
    assert(pf_tcp_cli_serving());
    assert(listener->accept(NULL, served, ERR_OK) == ERR_OK && pf_tcp_cli_connected());
    pf_tcp_cli_set_serving(false);
    assert(served->closed && strstr(served->written, "unavailable") != NULL && !pf_tcp_cli_connected());
    assert(!pf_tcp_cli_serving());
    assert(listener->accept(NULL, refused, ERR_OK) == ERR_OK && refused->closed && !pf_tcp_cli_connected());
    assert(strstr(refused->written, "unavailable") != NULL);
    pf_tcp_cli_set_serving(true);
    assert(pf_tcp_cli_serving());
    assert(listener->accept(NULL, restored, ERR_OK) == ERR_OK && pf_tcp_cli_connected() && !restored->closed);
    return 0;
}
"""


def test_tcp_cli_single_session_window_and_backpressure(tmp_path):
    (tmp_path / "lwip").mkdir()
    (tmp_path / "lwip/tcp.h").write_text(FAKE_LWIP)
    harness = tmp_path / "harness.c"
    harness.write_text(HARNESS)
    binary = tmp_path / "tcp_cli"
    compiled = subprocess.run([
        "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(tmp_path), "-I", str(ROOT / "firmware/src"),
        str(harness), str(ROOT / "firmware/src/app/wireless/wifi/tcp_cli.c"), "-o", str(binary),
    ], check=False, capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stderr
    subprocess.run([str(binary)], check=True)
