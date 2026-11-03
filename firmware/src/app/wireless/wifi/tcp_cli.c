/**
 * @file tcp_cli.c
 * @brief Single-client raw TCP transport for the shared CLI, built on the lwIP raw API in poll mode.
 */
#include "app/wireless/wifi/tcp_cli.h"

#include <string.h>

#include "lwip/tcp.h"
#include "app/wireless/wireless_auth.h"
#include "pico/time.h"
#include "platform/logging.h"

/** Final line sent to an extra client before it is closed. */
#define TCP_CLI_BUSY_NOTICE "PicoForge CLI busy: another TCP session is active.\r\n"
#define TCP_CLI_AUTH_REQUIRED_NOTICE "PicoForge CLI authentication required.\r\n"
#define TCP_CLI_AUTH_FAILED_NOTICE "PicoForge CLI authentication failed.\r\n"
#define TCP_CLI_AUTH_TIMEOUT_NOTICE "PicoForge CLI authentication timed out.\r\n"
#define TCP_CLI_IDLE_TIMEOUT_NOTICE "PicoForge CLI idle session timed out.\r\n"
#define TCP_CLI_AUTH_TIMEOUT_US 10000000u /**< Time allowed to answer a challenge. */
#define TCP_CLI_AUTH_RETRY_DELAY_US 2000000u /**< Cool down after a rejected proof. */
#define TCP_CLI_IDLE_TIMEOUT_US 900000000u /**< Release an abandoned single-client slot after 15 minutes. */
#define TCP_CLI_RETRY_DELAY_US 5000000u /**< Retry listener allocation without flooding logs. */
#define TCP_CLI_KEEPALIVE_IDLE_MS 30000u /**< Idle time before the first keepalive probe. */
#define TCP_CLI_KEEPALIVE_INTERVAL_MS 5000u /**< Time between unanswered keepalive probes. */
#define TCP_CLI_KEEPALIVE_COUNT 4u /**< Unanswered probes before lwIP drops a vanished client. */

static struct tcp_pcb *listener; /**< Listening PCB, or NULL when the listener is not running. */
static struct tcp_pcb *client; /**< Connected client PCB; NULL when idle or after lwIP freed it on error. */
static bool auth_required; /**< TCP CLI sessions need a one-use proof made with the configured BLE key. */
static bool auth_pending; /**< Accepted client has not yet proved possession of the key. */
static uint8_t auth_nonce[16]; /**< One-use challenge for the current client. */
static uint8_t auth_line[40]; /**< Bounded `AUTH <32 hex digits>\\r\\n` input. */
static uint8_t auth_line_length; /**< Bytes collected for auth_line. */
static uint64_t auth_deadline_us; /**< Deadline for the client proof. */
static uint64_t auth_retry_us; /**< Earliest next challenge after a rejected proof. */
static uint64_t client_activity_us; /**< Last received or sent client data. */
static uint64_t listener_retry_us; /**< Earliest next listener allocation attempt. */
/**
 * Received bytes not yet read by the CLI. Bytes are acknowledged to the window only after they are read, so the
 * peer can never have more than TCP_WND unread bytes outstanding and every accepted segment fits.
 */
static uint8_t rx_ring[TCP_WND];
static uint32_t rx_head; /**< Ring write index. */
static uint32_t rx_tail; /**< Ring read index. */
static uint32_t rx_count; /**< Unread bytes in rx_ring. */
static uint32_t rx_consumed; /**< Read bytes not yet reported to lwIP with tcp_recved(). */

/** @brief Fixed request binding the proof to TCP CLI session admission. */
static const uint8_t tcp_auth_request[9] = {'T', 'C', 'P', 'C', 'L', 'I', '0', '0', '1'};
/** @brief Protocol domain separating TCP CLI proofs from BLE control proofs. */
static const uint8_t tcp_auth_domain[9] = {'P', 'F', 'T', 'C', 'P', 'C', 'L', 'I', '1'};

/** @brief Encode length bytes as lowercase hex into a caller-owned buffer of at least twice that size. */
static void hex_encode(uint8_t *destination, const uint8_t *source, size_t length)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0u; i < length; ++i) {
        destination[2u * i] = (uint8_t)digits[source[i] >> 4u];
        destination[2u * i + 1u] = (uint8_t)digits[source[i] & 0x0fu];
    }
}

/** @brief Decode one hexadecimal digit; return -1 for non-hex input. */
static int8_t hex_value(uint8_t character)
{
    if (character >= '0' && character <= '9') return (int8_t)(character - '0');
    if (character >= 'a' && character <= 'f') return (int8_t)(character - 'a' + 10);
    if (character >= 'A' && character <= 'F') return (int8_t)(character - 'A' + 10);
    return -1;
}

/** @brief Invalidate the current challenge and discard any partial proof. */
static void reset_auth(void)
{
    auth_pending = false;
    auth_line_length = 0u;
    auth_deadline_us = 0u;
    memset(auth_nonce, 0, sizeof(auth_nonce));
    memset(auth_line, 0, sizeof(auth_line));
}

/** @brief Queue a fresh board-bound challenge and start the proof deadline; false on transport failure. */
static bool start_authentication(struct tcp_pcb *pcb)
{
    static const char prefix[] = "PicoForge AUTH ";
    static const char ending[] = "\r\n";
    uint8_t challenge[sizeof(prefix) - 1u + 32u + 1u + 16u + sizeof(ending) - 1u];
    uint8_t uid[8];
    size_t cursor = 0u;
    memcpy(challenge + cursor, prefix, sizeof(prefix) - 1u);
    cursor += sizeof(prefix) - 1u;
    if (!pf_wireless_auth_new_nonce(auth_nonce)) return false;
    pf_wireless_auth_device_id(uid);
    hex_encode(challenge + cursor, auth_nonce, sizeof(auth_nonce));
    cursor += 2u * sizeof(auth_nonce);
    challenge[cursor++] = ' ';
    hex_encode(challenge + cursor, uid, sizeof(uid));
    cursor += 2u * sizeof(uid);
    memcpy(challenge + cursor, ending, sizeof(ending) - 1u);
    cursor += sizeof(ending) - 1u;
    auth_pending = true;
    auth_deadline_us = time_us_64() + TCP_CLI_AUTH_TIMEOUT_US;
    return cursor <= tcp_sndbuf(pcb) && tcp_write(pcb, challenge, (u16_t)cursor, TCP_WRITE_FLAG_COPY) == ERR_OK &&
        tcp_output(pcb) == ERR_OK;
}

/** @brief Verify a complete AUTH line against the current one-use challenge. */
static bool verify_auth_line(void)
{
    static const uint8_t prefix[] = "AUTH ";
    uint8_t supplied[16];
    if (auth_line_length != sizeof(prefix) - 1u + 32u + 2u ||
        memcmp(auth_line, prefix, sizeof(prefix) - 1u) != 0 ||
        auth_line[auth_line_length - 2u] != '\r' || auth_line[auth_line_length - 1u] != '\n') return false;
    for (size_t i = 0u; i < sizeof(supplied); ++i) {
        const int8_t high = hex_value(auth_line[sizeof(prefix) - 1u + 2u * i]);
        const int8_t low = hex_value(auth_line[sizeof(prefix) + 2u * i]);
        if (high < 0 || low < 0) return false;
        supplied[i] = (uint8_t)((uint8_t)high << 4u | (uint8_t)low);
    }
    const bool accepted = pf_wireless_auth_verify(auth_nonce, tcp_auth_domain, tcp_auth_request, supplied);
    memset(supplied, 0, sizeof(supplied));
    return accepted;
}

/** @brief Copy already-admitted CLI bytes into the receive ring; the caller checks capacity before consuming auth. */
static void queue_input(struct pbuf *buffer, u16_t offset, u16_t length)
{
    uint32_t first = sizeof(rx_ring) - rx_head;
    if (first > length) first = length;
    (void)pbuf_copy_partial(buffer, rx_ring + rx_head, (u16_t)first, offset);
    (void)pbuf_copy_partial(buffer, rx_ring, (u16_t)(length - first), (u16_t)(offset + first));
    rx_head = (rx_head + length) % sizeof(rx_ring);
    rx_count += length;
}

/** @brief Forget all client input state. */
static void reset_input(void)
{
    rx_head = rx_tail = rx_count = rx_consumed = 0u;
}

/**
 * @brief Detach callbacks and close a PCB, aborting it if lwIP cannot queue the FIN.
 * @return True when the PCB was aborted, which lwIP callbacks must report as ERR_ABRT.
 */
static bool close_pcb(struct tcp_pcb *pcb)
{
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_err(pcb, NULL);
    if (tcp_close(pcb) != ERR_OK) {
        tcp_abort(pcb);
        return true;
    }
    return false;
}

/** @brief Send a short notice to a PCB and close it. @return True when the PCB was aborted. */
static bool close_with_notice(struct tcp_pcb *pcb, const char *notice)
{
    if (notice != NULL) {
        const size_t length = strlen(notice);
        if (length <= tcp_sndbuf(pcb) && tcp_write(pcb, notice, (u16_t)length, TCP_WRITE_FLAG_COPY) == ERR_OK) {
            (void)tcp_output(pcb);
        }
    }
    return close_pcb(pcb);
}

/** @brief lwIP error callback: the client PCB is already freed, so only drop the reference. */
static void client_error(void *argument, err_t error)
{
    (void)argument;
    PF_LOGGING_INFO("tcp-cli", "Client connection lost: %d", (int)error);
    client = NULL;
    reset_input();
    reset_auth();
}

/** @brief lwIP receive callback: copy a segment into rx_ring, or close the session on peer FIN. */
static err_t client_receive(void *argument, struct tcp_pcb *pcb, struct pbuf *buffer, err_t error)
{
    (void)argument;
    if (buffer == NULL) {
        if (pcb == client) {
            client = NULL;
            reset_input();
            reset_auth();
        }
        return close_pcb(pcb) ? ERR_ABRT : ERR_OK;
    }
    if (error != ERR_OK || pcb != client) {
        pbuf_free(buffer);
        return ERR_OK;
    }
    /* Refusal must not consume authentication state or acknowledge bytes: lwIP redelivers the whole pbuf. */
    if (buffer->tot_len > sizeof(rx_ring) - rx_count) return ERR_MEM;
    u16_t cli_offset = 0u;
    if (auth_pending) {
        u16_t consumed = buffer->tot_len;
        const size_t available = sizeof(auth_line) - auth_line_length;
        if (consumed > available) consumed = (u16_t)available;
        uint8_t *chunk = auth_line + auth_line_length;
        (void)pbuf_copy_partial(buffer, chunk, consumed, 0u);
        const uint8_t *newline = memchr(chunk, '\n', consumed);
        if (newline != NULL) consumed = (u16_t)(newline - chunk + 1u);
        auth_line_length += consumed;
        bool rejected = newline == NULL && buffer->tot_len > consumed;
        if (newline != NULL) {
            rejected = !verify_auth_line();
            if (!rejected) {
                reset_auth();
                cli_offset = consumed;
                PF_LOGGING_INFO("tcp-cli", "Client %s authenticated", ipaddr_ntoa(&pcb->remote_ip));
            }
        }
        if (rejected) {
            PF_LOGGING_WARNING("tcp-cli", "Client %s authentication failed", ipaddr_ntoa(&pcb->remote_ip));
            auth_retry_us = time_us_64() + TCP_CLI_AUTH_RETRY_DELAY_US;
            pbuf_free(buffer);
            client = NULL;
            reset_input();
            reset_auth();
            return close_with_notice(pcb, TCP_CLI_AUTH_FAILED_NOTICE) ? ERR_ABRT : ERR_OK;
        }
        if (consumed != 0u) tcp_recved(pcb, consumed);
        if (auth_pending) {
            pbuf_free(buffer);
            return ERR_OK;
        }
    }
    client_activity_us = time_us_64();
    const u16_t cli_length = (u16_t)(buffer->tot_len - cli_offset);
    queue_input(buffer, cli_offset, cli_length);
    pbuf_free(buffer);
    return ERR_OK;
}

/** @brief lwIP accept callback: adopt the first client, turn extra clients away with a notice. */
static err_t accept_client(void *argument, struct tcp_pcb *pcb, err_t error)
{
    (void)argument;
    if (error != ERR_OK || pcb == NULL) {
        return ERR_VAL;
    }
    if (client != NULL) {
        return close_with_notice(pcb, TCP_CLI_BUSY_NOTICE) ? ERR_ABRT : ERR_OK;
    }
    if (auth_required && time_us_64() < auth_retry_us) {
        PF_LOGGING_WARNING("tcp-cli", "Client %s authentication cooldown", ipaddr_ntoa(&pcb->remote_ip));
        return close_with_notice(pcb, TCP_CLI_AUTH_FAILED_NOTICE) ? ERR_ABRT : ERR_OK;
    }
    client = pcb;
    reset_input();
    reset_auth();
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, client_receive);
    tcp_err(pcb, client_error);
    tcp_nagle_disable(pcb);
    pcb->so_options |= SOF_KEEPALIVE;
    pcb->keep_idle = TCP_CLI_KEEPALIVE_IDLE_MS;
    pcb->keep_intvl = TCP_CLI_KEEPALIVE_INTERVAL_MS;
    pcb->keep_cnt = TCP_CLI_KEEPALIVE_COUNT;
    if (auth_required && !start_authentication(pcb)) {
        PF_LOGGING_WARNING("tcp-cli", "Client %s authentication challenge failed", ipaddr_ntoa(&pcb->remote_ip));
        client = NULL;
        reset_auth();
        return close_with_notice(pcb, TCP_CLI_AUTH_REQUIRED_NOTICE) ? ERR_ABRT : ERR_OK;
    }
    client_activity_us = time_us_64();
    PF_LOGGING_INFO("tcp-cli", "Client %s connected%s", ipaddr_ntoa(&pcb->remote_ip),
                   auth_required ? "; authentication pending" : "");
    return ERR_OK;
}

/** @brief Start listening on all IPv4 addresses. */
static bool start_listener(void)
{
    if (listener != NULL) {
        return true;
    }
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb == NULL) {
        PF_LOGGING_ERROR("tcp-cli", "Listener allocation failed");
        return false;
    }
    if (tcp_bind(pcb, IP_ANY_TYPE, PF_TCP_CLI_PORT) != ERR_OK) {
        PF_LOGGING_ERROR("tcp-cli", "Listener bind to port %u failed", (unsigned)PF_TCP_CLI_PORT);
        (void)close_pcb(pcb);
        return false;
    }
    listener = tcp_listen_with_backlog(pcb, 1u);
    if (listener == NULL) {
        PF_LOGGING_ERROR("tcp-cli", "Listen on port %u failed", (unsigned)PF_TCP_CLI_PORT);
        (void)close_pcb(pcb);
        return false;
    }
    tcp_accept(listener, accept_client);
    PF_LOGGING_INFO("tcp-cli", "Listening on TCP port %u", (unsigned)PF_TCP_CLI_PORT);
    return true;
}

/** @brief Stop accepting clients and close the active session/listener. */
static void stop_listener(void)
{
    pf_tcp_cli_close(NULL);
    struct tcp_pcb *pcb = listener;
    listener = NULL;
    if (pcb != NULL) {
        tcp_accept(pcb, NULL);
        (void)close_pcb(pcb);
    }
}

/** @copydoc pf_tcp_cli_serving */
bool pf_tcp_cli_serving(void)
{
    return listener != NULL;
}

/** @copydoc pf_tcp_cli_connected */
bool pf_tcp_cli_connected(void)
{
    return client != NULL && !auth_pending;
}

/** @copydoc pf_tcp_cli_read */
bool pf_tcp_cli_read(char *character)
{
    if (client == NULL || auth_pending || rx_count == 0u) {
        return false;
    }
    *character = (char)rx_ring[rx_tail];
    rx_tail = (rx_tail + 1u) % sizeof(rx_ring);
    --rx_count;
    ++rx_consumed;
    return true;
}

/** @copydoc pf_tcp_cli_write_available */
uint32_t pf_tcp_cli_write_available(void)
{
    if (client == NULL || auth_pending || tcp_sndqueuelen(client) >= TCP_SND_QUEUELEN) {
        return 0u;
    }
    return tcp_sndbuf(client);
}

/** @copydoc pf_tcp_cli_write */
uint32_t pf_tcp_cli_write(const char *data, uint32_t size)
{
    const uint32_t available = pf_tcp_cli_write_available();
    if (size > available) {
        size = available;
    }
    if (size == 0u || tcp_write(client, data, (u16_t)size, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        return 0u;
    }
    client_activity_us = time_us_64();
    return size;
}

/** @copydoc pf_tcp_cli_flush */
void pf_tcp_cli_flush(void)
{
    if (client == NULL) {
        return;
    }
    if (rx_consumed > 0u) {
        tcp_recved(client, (u16_t)rx_consumed);
        rx_consumed = 0u;
        client_activity_us = time_us_64();
    }
    (void)tcp_output(client);
}

/** @copydoc pf_tcp_cli_close */
void pf_tcp_cli_close(const char *notice)
{
    struct tcp_pcb *pcb = client;
    client = NULL;
    reset_input();
    reset_auth();
    if (pcb != NULL) {
        (void)close_with_notice(pcb, notice);
        PF_LOGGING_INFO("tcp-cli", "Client closed");
    }
}

/** @copydoc pf_tcp_cli_set_auth_required */
bool pf_tcp_cli_set_auth_required(bool required)
{
    if (required && !pf_wireless_auth_available()) return false;
    auth_required = required;
    return true;
}

/** @copydoc pf_tcp_cli_auth_required */
bool pf_tcp_cli_auth_required(void)
{
    return auth_required;
}

/** @copydoc pf_tcp_cli_poll */
void pf_tcp_cli_poll(bool serving, uint64_t now)
{
    if (!serving) {
        stop_listener();
        return;
    }
    if (listener == NULL && now >= listener_retry_us && !start_listener()) {
        listener_retry_us = now + TCP_CLI_RETRY_DELAY_US;
    }
    if (client != NULL && auth_pending && now >= auth_deadline_us) {
        PF_LOGGING_WARNING("tcp-cli", "Client %s authentication timed out", ipaddr_ntoa(&client->remote_ip));
        pf_tcp_cli_close(TCP_CLI_AUTH_TIMEOUT_NOTICE);
    } else if (client != NULL && !auth_pending && now - client_activity_us >= TCP_CLI_IDLE_TIMEOUT_US) {
        PF_LOGGING_INFO("tcp-cli", "Client %s idle session timed out", ipaddr_ntoa(&client->remote_ip));
        pf_tcp_cli_close(TCP_CLI_IDLE_TIMEOUT_NOTICE);
    }
}
