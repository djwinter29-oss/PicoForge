#include "unity.h"
#include <stdarg.h>
#include <string.h>

TEST_INCLUDE_PATH("firmware/tests/support/lwip")
#include "app/wireless/wifi/tcp_cli.c"

static struct tcp_pcb listen_pcb, client_pcb, extra_pcb;
static tcp_accept_fn accept_callback;
static tcp_recv_fn recv_callback;
static tcp_err_fn err_callback;
static err_t close_result;

bool pf_logging_write(pf_logging_level_t level, const char *tag, const char *format, ...)
{
    (void)level; (void)tag; (void)format;
    return true;
}
struct tcp_pcb *tcp_new_ip_type(u8_t type) { (void)type; return &listen_pcb; }
err_t tcp_bind(struct tcp_pcb *pcb, const void *ipaddr, u16_t port)
{
    (void)pcb; (void)ipaddr;
    TEST_ASSERT_EQUAL_UINT16(PF_TCP_CLI_PORT, port);
    return ERR_OK;
}
struct tcp_pcb *tcp_listen_with_backlog(struct tcp_pcb *pcb, u8_t backlog) { (void)backlog; return pcb; }
void tcp_arg(struct tcp_pcb *pcb, void *arg) { (void)pcb; (void)arg; }
void tcp_accept(struct tcp_pcb *pcb, tcp_accept_fn accept) { (void)pcb; accept_callback = accept; }
void tcp_recv(struct tcp_pcb *pcb, tcp_recv_fn recv) { (void)pcb; recv_callback = recv; }
void tcp_err(struct tcp_pcb *pcb, tcp_err_fn err) { (void)pcb; err_callback = err; }
err_t tcp_write(struct tcp_pcb *pcb, const void *data, u16_t len, u8_t apiflags)
{
    TEST_ASSERT_EQUAL_UINT8(TCP_WRITE_FLAG_COPY, apiflags);
    memcpy(pcb->sent + pcb->sent_size, data, len);
    pcb->sent_size += len;
    pcb->snd_buf -= len;
    return ERR_OK;
}
err_t tcp_output(struct tcp_pcb *pcb) { (void)pcb; return ERR_OK; }
void tcp_recved(struct tcp_pcb *pcb, u16_t len) { pcb->recved += len; }
err_t tcp_close(struct tcp_pcb *pcb)
{
    pcb->closed = (close_result == ERR_OK);
    return close_result;
}
void tcp_abort(struct tcp_pcb *pcb) { pcb->aborted = true; }
u16_t pbuf_copy_partial(const struct pbuf *p, void *dataptr, u16_t len, u16_t offset)
{
    memcpy(dataptr, (const char *)p->payload + offset, len);
    return len;
}
u8_t pbuf_free(struct pbuf *p) { (void)p; return 1u; }

static err_t deliver(struct tcp_pcb *pcb, const char *text)
{
    static struct pbuf buffer;
    buffer = (struct pbuf){.tot_len = (u16_t)strlen(text), .payload = text};
    return recv_callback(NULL, pcb, &buffer, ERR_OK);
}

void setUp(void)
{
    listener = client = NULL;
    reset_input();
    memset(&listen_pcb, 0, sizeof(listen_pcb));
    memset(&client_pcb, 0, sizeof(client_pcb));
    memset(&extra_pcb, 0, sizeof(extra_pcb));
    client_pcb.snd_buf = extra_pcb.snd_buf = sizeof(client_pcb.sent);
    close_result = ERR_OK;
    TEST_ASSERT_TRUE(pf_tcp_cli_start());
    TEST_ASSERT_EQUAL_INT(ERR_OK, accept_callback(NULL, &client_pcb, ERR_OK));
}

void tearDown(void) {}

void test_accepted_client_reads_input_and_credits_the_window_only_after_consumption(void)
{
    TEST_ASSERT_TRUE(pf_tcp_cli_connected());
    TEST_ASSERT_TRUE(client_pcb.nodelay && (client_pcb.so_options & SOF_KEEPALIVE));
    TEST_ASSERT_EQUAL_INT(ERR_OK, deliver(&client_pcb, "help\r"));
    pf_tcp_cli_flush();
    TEST_ASSERT_EQUAL_UINT32(0u, client_pcb.recved);

    char text[6] = {0};
    for (size_t index = 0u; index < 5u; ++index) {
        TEST_ASSERT_TRUE(pf_tcp_cli_read(&text[index]));
    }
    TEST_ASSERT_FALSE(pf_tcp_cli_read(&text[5]));
    TEST_ASSERT_EQUAL_STRING("help\r", text);
    pf_tcp_cli_flush();
    TEST_ASSERT_EQUAL_UINT32(5u, client_pcb.recved);
}

void test_receive_ring_wraps_and_refuses_segments_that_do_not_fit(void)
{
    char block[TCP_WND - 4u + 1u];
    memset(block, 'a', sizeof(block) - 1u);
    block[sizeof(block) - 1u] = '\0';
    TEST_ASSERT_EQUAL_INT(ERR_OK, deliver(&client_pcb, block));
    TEST_ASSERT_EQUAL_INT(ERR_MEM, deliver(&client_pcb, "12345"));
    char character;
    for (size_t index = 0u; index < sizeof(block) - 1u; ++index) {
        TEST_ASSERT_TRUE(pf_tcp_cli_read(&character));
    }
    TEST_ASSERT_EQUAL_INT(ERR_OK, deliver(&client_pcb, "wrap!xyz"));
    char text[9] = {0};
    for (size_t index = 0u; index < 8u; ++index) {
        TEST_ASSERT_TRUE(pf_tcp_cli_read(&text[index]));
    }
    TEST_ASSERT_EQUAL_STRING("wrap!xyz", text);
}

void test_second_client_gets_a_busy_notice_and_is_closed(void)
{
    TEST_ASSERT_EQUAL_INT(ERR_OK, accept_callback(NULL, &extra_pcb, ERR_OK));
    TEST_ASSERT_TRUE(extra_pcb.closed);
    TEST_ASSERT_EQUAL_STRING_LEN(TCP_CLI_BUSY_NOTICE, extra_pcb.sent, extra_pcb.sent_size);
    TEST_ASSERT_TRUE(pf_tcp_cli_connected());
}

void test_writes_respect_send_space_and_queue_limits(void)
{
    client_pcb.snd_buf = 3u;
    TEST_ASSERT_EQUAL_UINT32(3u, pf_tcp_cli_write("hello", 5u));
    TEST_ASSERT_EQUAL_UINT32(0u, pf_tcp_cli_write("lo", 2u));
    client_pcb.snd_buf = 10u;
    client_pcb.snd_queuelen = TCP_SND_QUEUELEN;
    TEST_ASSERT_EQUAL_UINT32(0u, pf_tcp_cli_write_available());
    TEST_ASSERT_EQUAL_STRING_LEN("hel", client_pcb.sent, client_pcb.sent_size);
}

void test_peer_close_error_and_local_close_release_the_client(void)
{
    TEST_ASSERT_EQUAL_INT(ERR_OK, recv_callback(NULL, &client_pcb, NULL, ERR_OK));
    TEST_ASSERT_FALSE(pf_tcp_cli_connected());
    TEST_ASSERT_TRUE(client_pcb.closed);

    memset(&client_pcb, 0, sizeof(client_pcb));
    TEST_ASSERT_EQUAL_INT(ERR_OK, accept_callback(NULL, &client_pcb, ERR_OK));
    err_callback(NULL, ERR_RST);
    TEST_ASSERT_FALSE(pf_tcp_cli_connected());
    TEST_ASSERT_FALSE(client_pcb.closed || client_pcb.aborted);

    TEST_ASSERT_EQUAL_INT(ERR_OK, accept_callback(NULL, &client_pcb, ERR_OK));
    close_result = ERR_MEM;
    pf_tcp_cli_close(NULL);
    TEST_ASSERT_FALSE(pf_tcp_cli_connected());
    TEST_ASSERT_TRUE(client_pcb.aborted);
}
