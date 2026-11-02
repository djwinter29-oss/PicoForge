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
static bool auth_accept;
static uint64_t fake_time_us;
static bool key_available;
static unsigned input_copies;

bool pf_logging_write(pf_logging_level_t level, const char *tag, const char *format, ...)
{
    (void)level; (void)tag; (void)format;
    return true;
}
uint64_t time_us_64(void) { return fake_time_us; }
void pf_wireless_auth_set_key(const uint8_t key[32])
{
    key_available = key != NULL && key[0] != 0u;
}
bool pf_wireless_auth_available(void) { return key_available; }
bool pf_wireless_auth_new_nonce(uint8_t nonce[16])
{
    for (size_t i = 0u; i < 16u; ++i) nonce[i] = (uint8_t)i;
    return true;
}
void pf_wireless_auth_device_id(uint8_t uid[8])
{
    for (size_t i = 0u; i < 8u; ++i) uid[i] = (uint8_t)i;
}
bool pf_wireless_auth_verify(const uint8_t nonce[16], const uint8_t domain[9], const uint8_t request[9],
                             const uint8_t proof[16])
{
    return auth_accept && memcmp(domain, "PFTCPCLI1", 9u) == 0 &&
        memcmp(request, "TCPCLI001", 9u) == 0 && memcmp(nonce, proof, 16u) == 0;
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
    if (len != 0u) ++input_copies;
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
    auth_accept = true;
    fake_time_us = 0u;
    input_copies = 0u;
    auth_retry_us = listener_retry_us = client_activity_us = 0u;
    reset_input();
    pf_wireless_auth_set_key(NULL);
    TEST_ASSERT_TRUE(pf_tcp_cli_set_auth_required(false));
    memset(&listen_pcb, 0, sizeof(listen_pcb));
    memset(&client_pcb, 0, sizeof(client_pcb));
    memset(&extra_pcb, 0, sizeof(extra_pcb));
    client_pcb.snd_buf = extra_pcb.snd_buf = sizeof(client_pcb.sent);
    close_result = ERR_OK;
    pf_tcp_cli_poll(true, fake_time_us);
    TEST_ASSERT_TRUE(pf_tcp_cli_serving());
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

void test_read_activity_is_stamped_once_when_consumed_input_is_flushed(void)
{
    TEST_ASSERT_EQUAL_INT(ERR_OK, deliver(&client_pcb, "help\r"));
    fake_time_us = 10u;
    char character;
    TEST_ASSERT_TRUE(pf_tcp_cli_read(&character));
    TEST_ASSERT_TRUE(pf_tcp_cli_read(&character));
    TEST_ASSERT_EQUAL_UINT64(0u, client_activity_us);

    fake_time_us = 20u;
    pf_tcp_cli_flush();
    TEST_ASSERT_EQUAL_UINT64(20u, client_activity_us);
    TEST_ASSERT_EQUAL_UINT32(2u, client_pcb.recved);
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

void test_lost_saved_link_closes_client_and_rejects_new_sessions_until_restored(void)
{
    pf_tcp_cli_poll(false, fake_time_us);
    TEST_ASSERT_FALSE(pf_tcp_cli_connected());
    TEST_ASSERT_FALSE(pf_tcp_cli_serving());
    TEST_ASSERT_TRUE(client_pcb.closed);
    TEST_ASSERT_EQUAL_UINT16(0u, client_pcb.sent_size);
}

void test_authentication_requires_a_nonzero_key_and_verifies_before_cli_io(void)
{
    uint8_t key[32] = {0x42u};
    uint8_t zero_key[32] = {0u};
    TEST_ASSERT_FALSE(pf_tcp_cli_set_auth_required(true));
    pf_wireless_auth_set_key(zero_key);
    TEST_ASSERT_FALSE(pf_tcp_cli_set_auth_required(true));
    pf_wireless_auth_set_key(key);
    TEST_ASSERT_TRUE(pf_tcp_cli_set_auth_required(true));
    pf_tcp_cli_close(NULL);
    memset(&client_pcb, 0, sizeof(client_pcb));
    client_pcb.snd_buf = sizeof(client_pcb.sent);
    TEST_ASSERT_EQUAL_INT(ERR_OK, accept_callback(NULL, &client_pcb, ERR_OK));
    TEST_ASSERT_FALSE(pf_tcp_cli_connected());
    TEST_ASSERT_EQUAL_STRING_LEN("PicoForge AUTH 000102030405060708090a0b0c0d0e0f 0001020304050607\r\n",
                                 client_pcb.sent, client_pcb.sent_size);
    TEST_ASSERT_FALSE(pf_tcp_cli_read(&(char){0}));
    input_copies = 0u;
    TEST_ASSERT_EQUAL_INT(ERR_OK, deliver(&client_pcb, "AUTH 000102030405060708090a0b0c0d0e0f\r\nhelp\r"));
    TEST_ASSERT_EQUAL_UINT32(2u, input_copies);
    TEST_ASSERT_TRUE(pf_tcp_cli_connected());
    char command[6] = {0};
    for (size_t i = 0u; i < 5u; ++i) TEST_ASSERT_TRUE(pf_tcp_cli_read(&command[i]));
    TEST_ASSERT_EQUAL_STRING("help\r", command);
}

void test_rejected_tcp_authentication_closes_the_client_with_a_notice(void)
{
    uint8_t key[32] = {0x42u};
    pf_wireless_auth_set_key(key);
    TEST_ASSERT_TRUE(pf_tcp_cli_set_auth_required(true));
    pf_tcp_cli_close(NULL);
    memset(&client_pcb, 0, sizeof(client_pcb));
    client_pcb.snd_buf = sizeof(client_pcb.sent);
    TEST_ASSERT_EQUAL_INT(ERR_OK, accept_callback(NULL, &client_pcb, ERR_OK));
    auth_accept = false;
    TEST_ASSERT_EQUAL_INT(ERR_OK, deliver(&client_pcb, "AUTH 00000000000000000000000000000000\r\n"));
    TEST_ASSERT_FALSE(pf_tcp_cli_connected());
    TEST_ASSERT_TRUE(client_pcb.closed);
    TEST_ASSERT_EQUAL_STRING_LEN(TCP_CLI_AUTH_FAILED_NOTICE, client_pcb.sent + 66u,
                                 client_pcb.sent_size - 66u);
    fake_time_us = 1000000u;
    struct tcp_pcb rejected = {0};
    rejected.snd_buf = sizeof(rejected.sent);
    TEST_ASSERT_EQUAL_INT(ERR_OK, accept_callback(NULL, &rejected, ERR_OK));
    TEST_ASSERT_TRUE(rejected.closed);
    fake_time_us = 2000000u;
}

void test_refused_auth_buffer_preserves_partial_proof_and_window_credit_until_redelivery(void)
{
    uint8_t key[32] = {0x42u};
    pf_wireless_auth_set_key(key);
    TEST_ASSERT_TRUE(pf_tcp_cli_set_auth_required(true));
    pf_tcp_cli_close(NULL);
    memset(&client_pcb, 0, sizeof(client_pcb));
    client_pcb.snd_buf = sizeof(client_pcb.sent);
    TEST_ASSERT_EQUAL_INT(ERR_OK, accept_callback(NULL, &client_pcb, ERR_OK));
    TEST_ASSERT_EQUAL_INT(ERR_OK, deliver(&client_pcb, "AUTH 000102"));
    const uint8_t partial_length = auth_line_length;
    rx_count = TCP_WND - 1u;
    TEST_ASSERT_EQUAL_INT(ERR_MEM, deliver(&client_pcb, "030405060708090a0b0c0d0e0f\r\nhelp\r"));
    TEST_ASSERT_TRUE(auth_pending);
    TEST_ASSERT_EQUAL_UINT8(partial_length, auth_line_length);
    TEST_ASSERT_EQUAL_UINT32(11u, client_pcb.recved);
    rx_count = 0u;
    TEST_ASSERT_EQUAL_INT(ERR_OK, deliver(&client_pcb, "030405060708090a0b0c0d0e0f\r\nhelp\r"));
    TEST_ASSERT_TRUE(pf_tcp_cli_connected());
    TEST_ASSERT_EQUAL_UINT32(39u, client_pcb.recved);
    char command[6] = {0};
    for (size_t i = 0u; i < 5u; ++i) TEST_ASSERT_TRUE(pf_tcp_cli_read(&command[i]));
    TEST_ASSERT_EQUAL_STRING("help\r", command);
}

void test_authentication_handles_every_proof_split_with_one_copy_per_chunk(void)
{
    uint8_t key[32] = {0x42u};
    const char proof_line[] = "AUTH 000102030405060708090a0b0c0d0e0f\r\n";
    pf_wireless_auth_set_key(key);
    TEST_ASSERT_TRUE(pf_tcp_cli_set_auth_required(true));
    for (size_t split = 1u; split < sizeof(proof_line) - 1u; ++split) {
        pf_tcp_cli_close(NULL);
        memset(&client_pcb, 0, sizeof(client_pcb));
        client_pcb.snd_buf = sizeof(client_pcb.sent);
        TEST_ASSERT_EQUAL_INT(ERR_OK, accept_callback(NULL, &client_pcb, ERR_OK));
        char prefix[sizeof(proof_line)] = {0};
        memcpy(prefix, proof_line, split);
        input_copies = 0u;
        TEST_ASSERT_EQUAL_INT(ERR_OK, deliver(&client_pcb, prefix));
        TEST_ASSERT_FALSE(pf_tcp_cli_connected());
        TEST_ASSERT_EQUAL_UINT32(split, client_pcb.recved);
        TEST_ASSERT_EQUAL_INT(ERR_OK, deliver(&client_pcb, proof_line + split));
        TEST_ASSERT_TRUE(pf_tcp_cli_connected());
        TEST_ASSERT_EQUAL_UINT32(39u, client_pcb.recved);
        TEST_ASSERT_EQUAL_UINT32(2u, input_copies);
        TEST_ASSERT_FALSE(pf_tcp_cli_read(&(char){0}));
    }
}

void test_authentication_rejects_malformed_and_overlong_lines_without_cli_input(void)
{
    uint8_t key[32] = {0x42u};
    const char *invalid[] = {
        "\nhelp\r",
        "AUTH 000102030405060708090a0b0c0d0e0f\n",
        "AUTH 000102030405060708090a0b0c0d0e0g\r\n",
        "AUTH 000102030405060708090a0b0c0d0e0fX\r\n",
        "AUTH 000102030405060708090a0b0c0d0e0fXXXX",
    };
    pf_wireless_auth_set_key(key);
    TEST_ASSERT_TRUE(pf_tcp_cli_set_auth_required(true));
    for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        pf_tcp_cli_close(NULL);
        fake_time_us += TCP_CLI_AUTH_RETRY_DELAY_US;
        memset(&client_pcb, 0, sizeof(client_pcb));
        client_pcb.snd_buf = sizeof(client_pcb.sent);
        TEST_ASSERT_EQUAL_INT(ERR_OK, accept_callback(NULL, &client_pcb, ERR_OK));
        input_copies = 0u;
        TEST_ASSERT_EQUAL_INT(ERR_OK, deliver(&client_pcb, invalid[i]));
        TEST_ASSERT_TRUE(client_pcb.closed);
        TEST_ASSERT_FALSE(pf_tcp_cli_connected());
        TEST_ASSERT_FALSE(pf_tcp_cli_read(&(char){0}));
        TEST_ASSERT_EQUAL_UINT32(0u, client_pcb.recved);
        TEST_ASSERT_EQUAL_UINT32(1u, input_copies);
    }
}

void test_full_auth_buffer_waits_for_next_byte_then_rejects_without_copying_more(void)
{
    uint8_t key[32] = {0x42u};
    pf_wireless_auth_set_key(key);
    TEST_ASSERT_TRUE(pf_tcp_cli_set_auth_required(true));
    pf_tcp_cli_close(NULL);
    memset(&client_pcb, 0, sizeof(client_pcb));
    client_pcb.snd_buf = sizeof(client_pcb.sent);
    TEST_ASSERT_EQUAL_INT(ERR_OK, accept_callback(NULL, &client_pcb, ERR_OK));
    TEST_ASSERT_EQUAL_INT(ERR_OK, deliver(&client_pcb, "AUTH 000102030405060708090a0b0c0d0e0fXXX"));
    TEST_ASSERT_TRUE(auth_pending);
    TEST_ASSERT_EQUAL_UINT32(40u, client_pcb.recved);
    input_copies = 0u;
    TEST_ASSERT_EQUAL_INT(ERR_OK, deliver(&client_pcb, "\nhelp\r"));
    TEST_ASSERT_TRUE(client_pcb.closed);
    TEST_ASSERT_FALSE(pf_tcp_cli_connected());
    TEST_ASSERT_EQUAL_UINT32(40u, client_pcb.recved);
    TEST_ASSERT_EQUAL_UINT32(0u, input_copies);
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
