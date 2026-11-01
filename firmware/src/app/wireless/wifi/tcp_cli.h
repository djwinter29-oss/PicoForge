/**
 * @file tcp_cli.h
 * @brief Raw TCP byte stream for the shared CLI session over Wi-Fi (for example PuTTY "Raw" mode).
 *
 * One client at a time is accepted on PF_TCP_CLI_PORT; extra clients get a busy notice and are closed. All functions
 * must run on the wireless owner task that calls cyw43_arch_poll(), so lwIP callbacks never race these calls.
 */
#ifndef PF_TCP_CLI_H
#define PF_TCP_CLI_H

#include <stdbool.h>
#include <stdint.h>

/** TCP port of the CLI listener. Not 23, so clients are not tempted to use Telnet negotiation. */
#define PF_TCP_CLI_PORT 2323u

/**
 * @brief Start listening on PF_TCP_CLI_PORT on every IPv4 address; safe to call before the link is up.
 * @return False when the listener could not be allocated or bound; the CLI then stays USB-only.
 */
bool pf_tcp_cli_start(void);

/** @brief Report whether a client connection is currently open. */
bool pf_tcp_cli_connected(void);

/**
 * @brief Take one received byte; the receive window reopens as bytes are consumed, giving natural backpressure.
 * @param character Destination for the byte.
 * @return False when no input is queued or no client is connected.
 */
bool pf_tcp_cli_read(char *character);

/** @brief Bytes pf_tcp_cli_write() can accept now; 0 when not connected or the send queue is full. */
uint32_t pf_tcp_cli_write_available(void);

/**
 * @brief Copy output into the TCP send buffer.
 * @param data Bytes to send.
 * @param size Byte count.
 * @return Bytes accepted, which may be fewer than size.
 */
uint32_t pf_tcp_cli_write(const char *data, uint32_t size);

/** @brief Push queued output and acknowledge consumed input to the peer. */
void pf_tcp_cli_flush(void);

/**
 * @brief Close the client connection, discarding unread input.
 * @param notice Optional final line queued before closing; NULL closes silently.
 */
void pf_tcp_cli_close(const char *notice);

#endif
