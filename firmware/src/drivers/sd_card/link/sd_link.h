#ifndef PF_SD_LINK_H
#define PF_SD_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Physical signaling mode, not an attached device's command protocol. */
typedef enum {
    PF_SD_LINK_SPI,       /**< Full-duplex single-lane SPI. */
    PF_SD_LINK_NATIVE_SD, /**< Separate bidirectional CMD and DAT signals. */
} pf_sd_link_mode_t;

/** Physical outcome; device drivers translate into their public results. */
typedef enum {
    PF_SD_LINK_OK,               /**< Requested operation completed. */
    PF_SD_LINK_INVALID_ARGUMENT, /**< Invalid handle or transfer shape. */
    PF_SD_LINK_NOT_READY,        /**< Resources unavailable. */
    PF_SD_LINK_UNSUPPORTED,      /**< Capability unavailable. */
    PF_SD_LINK_IO_ERROR,         /**< Hardware transfer failed. */
    PF_SD_LINK_TIMEOUT,          /**< Bounded wait expired. */
} pf_sd_link_result_t;

/** Raw serial capability, defined in serial.h. */
typedef struct pf_sd_link_serial_ops pf_sd_link_serial_ops_t;
/** SD CMD/DAT wire capability, defined in sd_wire.h. */
typedef struct pf_sd_link_wire_ops pf_sd_link_wire_ops_t;

/** Common lifecycle and platform services; calls are externally serialized. */
typedef struct {
    pf_sd_link_result_t (*init)(void *context); /**< Acquire resources; no device commands. */
    void (*deinit)(void *context); /**< Release resources; never sync the device. */
    void (*abort)(void *context); /**< Stop hardware; never replay transfers. */
    pf_sd_link_result_t (*set_clock)(void *context, uint32_t requested_hz,
                                        uint32_t *actual_hz); /**< Set and report calculated clock. */
    const pf_sd_link_serial_ops_t *serial; /**< Optional raw serial operations. */
    const pf_sd_link_wire_ops_t *sd_wire; /**< Optional SD CMD/DAT wire operations. */
    uint64_t (*time_us)(void); /**< Wrapping monotonic platform microseconds. */
    void (*delay_ms)(uint32_t duration); /**< Platform millisecond delay. */
    bool (*enable_wait)(void *context); /**< Install optional bounded scheduler waits. */
} pf_sd_link_ops_t;

/** Non-owning descriptor; context and operations must outlive all access. */
typedef struct {
    void *context; /**< Backend-owned configuration and hardware state. */
    const pf_sd_link_ops_t *ops; /**< Immutable implementation. */
    pf_sd_link_mode_t mode; /**< Physical wire interface. */
    bool (*present)(const void *context); /**< Optional board-owned presence detector. */
    const void *presence_context; /**< Borrowed immutable detector argument. */
} pf_sd_link_t;

/** Acquire a configured backend without identifying or accessing media. */
pf_sd_link_result_t pf_sd_link_init(const pf_sd_link_t *bus);
/** Release resources after the device owner completes protocol shutdown. */
void pf_sd_link_deinit(const pf_sd_link_t *bus);
/** Stop hardware; interrupted writes may already have taken effect. */
void pf_sd_link_abort(const pf_sd_link_t *bus);
/** Set wire clock, reporting the calculated actual rate on success. */
pf_sd_link_result_t pf_sd_link_set_clock(const pf_sd_link_t *bus,
                                                uint32_t requested_hz, uint32_t *actual_hz);
/** Validate common lifecycle/services for a mode; wire capabilities validate their own operations. */
bool pf_sd_link_valid(const pf_sd_link_t *bus, pf_sd_link_mode_t mode);
/** Compare immutable descriptors without inspecting backend state. */
bool pf_sd_link_same(const pf_sd_link_t *left, const pf_sd_link_t *right);
/** Return whether a board presence detector is installed. */
bool pf_sd_link_presence_known(const pf_sd_link_t *bus);
/** Sample presence; absent a detector, assume media is present. */
bool pf_sd_link_present(const pf_sd_link_t *bus);
/** Read microseconds; requires a validated descriptor with a time callback. */
uint64_t pf_sd_link_time_us(const pf_sd_link_t *bus);
/** Delay milliseconds; requires a validated descriptor with a delay callback. */
void pf_sd_link_delay_ms(const pf_sd_link_t *bus, uint32_t duration);
/** Install optional bounded waits; false means unavailable or invalid. */
bool pf_sd_link_enable_wait(const pf_sd_link_t *bus);

#endif
