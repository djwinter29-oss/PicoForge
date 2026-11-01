/**
 * @file tcp_cli.c
 * @brief Single-client raw TCP transport for the shared CLI, built on the lwIP raw API in poll mode.
 */
#include "app/wireless/wifi/tcp_cli.h"

#include <string.h>

#include "lwip/tcp.h"
#include "platform/logging.h"

/** Final line sent to an extra client before it is closed. */
#define TCP_CLI_BUSY_NOTICE "PicoForge CLI busy: another TCP session is active.\r\n"
/** Final line sent when the CLI stops serving TCP because the saved Wi-Fi link is down or replaced by a test. */
#define TCP_CLI_UNAVAILABLE_NOTICE "PicoForge CLI unavailable: saved Wi-Fi link is not active.\r\n"
#define TCP_CLI_KEEPALIVE_IDLE_MS 30000u /**< Idle time before the first keepalive probe. */
#define TCP_CLI_KEEPALIVE_INTERVAL_MS 5000u /**< Time between unanswered keepalive probes. */
#define TCP_CLI_KEEPALIVE_COUNT 4u /**< Unanswered probes before lwIP drops a vanished client. */

static struct tcp_pcb *listener; /**< Listening PCB, or NULL when the listener is not running. */
static struct tcp_pcb *client; /**< Connected client PCB; NULL when idle or after lwIP freed it on error. */
static bool serving = true; /**< False turns every client away; set by the wireless owner from the link state. */
/**
 * Received bytes not yet read by the CLI. Bytes are acknowledged to the window only after they are read, so the
 * peer can never have more than TCP_WND unread bytes outstanding and every accepted segment fits.
 */
static uint8_t rx_ring[TCP_WND];
static uint32_t rx_head; /**< Ring write index. */
static uint32_t rx_tail; /**< Ring read index. */
static uint32_t rx_count; /**< Unread bytes in rx_ring. */
static uint32_t rx_consumed; /**< Read bytes not yet reported to lwIP with tcp_recved(). */

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
}

/** @brief lwIP receive callback: copy a segment into rx_ring, or close the session on peer FIN. */
static err_t client_receive(void *argument, struct tcp_pcb *pcb, struct pbuf *buffer, err_t error)
{
    (void)argument;
    if (buffer == NULL) {
        if (pcb == client) {
            client = NULL;
            reset_input();
        }
        return close_pcb(pcb) ? ERR_ABRT : ERR_OK;
    }
    if (error != ERR_OK || pcb != client) {
        pbuf_free(buffer);
        return ERR_OK;
    }
    if (buffer->tot_len > sizeof(rx_ring) - rx_count) {
        return ERR_MEM; /* Unreachable while the window invariant holds; lwIP redelivers refused data later. */
    }
    uint32_t first = sizeof(rx_ring) - rx_head;
    if (first > buffer->tot_len) {
        first = buffer->tot_len;
    }
    (void)pbuf_copy_partial(buffer, rx_ring + rx_head, (u16_t)first, 0u);
    (void)pbuf_copy_partial(buffer, rx_ring, (u16_t)(buffer->tot_len - first), (u16_t)first);
    rx_head = (rx_head + buffer->tot_len) % sizeof(rx_ring);
    rx_count += buffer->tot_len;
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
    if (!serving || client != NULL) {
        return close_with_notice(pcb, serving ? TCP_CLI_BUSY_NOTICE : TCP_CLI_UNAVAILABLE_NOTICE) ? ERR_ABRT : ERR_OK;
    }
    client = pcb;
    reset_input();
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, client_receive);
    tcp_err(pcb, client_error);
    tcp_nagle_disable(pcb);
    pcb->so_options |= SOF_KEEPALIVE;
    pcb->keep_idle = TCP_CLI_KEEPALIVE_IDLE_MS;
    pcb->keep_intvl = TCP_CLI_KEEPALIVE_INTERVAL_MS;
    pcb->keep_cnt = TCP_CLI_KEEPALIVE_COUNT;
    PF_LOGGING_INFO("tcp-cli", "Client connected");
    return ERR_OK;
}

/** @copydoc pf_tcp_cli_start */
bool pf_tcp_cli_start(void)
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

/** @copydoc pf_tcp_cli_set_serving */
void pf_tcp_cli_set_serving(bool enabled)
{
    serving = enabled;
    if (!enabled) pf_tcp_cli_close(TCP_CLI_UNAVAILABLE_NOTICE);
}

/** @copydoc pf_tcp_cli_serving */
bool pf_tcp_cli_serving(void)
{
    return listener != NULL && serving;
}

/** @copydoc pf_tcp_cli_connected */
bool pf_tcp_cli_connected(void)
{
    return client != NULL;
}

/** @copydoc pf_tcp_cli_read */
bool pf_tcp_cli_read(char *character)
{
    if (client == NULL || rx_count == 0u) {
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
    if (client == NULL || tcp_sndqueuelen(client) >= TCP_SND_QUEUELEN) {
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
    }
    (void)tcp_output(client);
}

/** @copydoc pf_tcp_cli_close */
void pf_tcp_cli_close(const char *notice)
{
    struct tcp_pcb *pcb = client;
    client = NULL;
    reset_input();
    if (pcb != NULL) {
        (void)close_with_notice(pcb, notice);
        PF_LOGGING_INFO("tcp-cli", "Client closed");
    }
}
