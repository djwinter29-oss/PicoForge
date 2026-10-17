#ifndef PF_STORAGE_BUS_H
#define PF_STORAGE_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Physical signaling mode, not the attached device's command protocol. */
typedef enum {
    PF_STORAGE_BUS_SPI,      /**< Clocked full-duplex single-lane SPI. */
    PF_STORAGE_BUS_SDIO_PIO, /**< Native SD command/data signaling. */
    PF_STORAGE_BUS_QSPI_PIO, /**< Single/quad serial signaling. */
} pf_storage_bus_mode_t;

/** Transport outcome; device protocol layers translate into their public results. */
typedef enum {
    PF_STORAGE_BUS_OK,               /**< Requested wire operation completed. */
    PF_STORAGE_BUS_INVALID_ARGUMENT, /**< Invalid handle, configuration, or transfer shape. */
    PF_STORAGE_BUS_NOT_READY,        /**< Backend has not acquired its hardware resources. */
    PF_STORAGE_BUS_UNSUPPORTED,      /**< Backend cannot perform this operation. */
    PF_STORAGE_BUS_IO_ERROR,         /**< Hardware transfer failed. */
    PF_STORAGE_BUS_TIMEOUT,          /**< A bounded hardware wait expired. */
} pf_storage_bus_result_t;

/** One CS-bounded serial transaction. Native SD uses its own typed wire API. */
typedef struct {
    uint8_t command;       /**< First byte transmitted with one data lane. */
    uint8_t address_bytes; /**< Number of big-endian address bytes, zero to three. */
    uint32_t address;      /**< Byte address fitting the selected address width. */
    uint8_t dummy_cycles; /**< Single-lane clock cycles before the data phase. */
    uint8_t data_lanes;   /**< One or four lanes for the payload. */
    const uint8_t *tx;     /**< Optional transmit payload, valid until return. */
    uint8_t *rx;           /**< Optional receive payload, valid until return. */
    size_t length;        /**< Payload byte count; zero permits command-only transactions. */
} pf_storage_serial_transaction_t;

/** Backend lifecycle; operations are synchronous and caller-serialized. */
typedef struct {
    pf_storage_bus_result_t (*init)(void *context); /**< Acquire resources and set idle pin levels. */
    void (*deinit)(void *context);                 /**< Release resources; never sync the device. */
    void (*abort)(void *context);                  /**< Stop hardware without device commands. */
    pf_storage_bus_result_t (*set_clock)(void *context, uint32_t requested_hz,
                                        uint32_t *actual_hz); /**< Program and report actual frequency. */
    pf_storage_bus_result_t (*transfer)(void *context,
        const pf_storage_serial_transaction_t *transaction); /**< NULL for native SD. */
} pf_storage_bus_ops_t;

/** Non-owning handle. Context and operations must outlive every synchronous call. */
typedef struct {
    void *context;                    /**< Backend-owned configuration and hardware state. */
    const pf_storage_bus_ops_t *ops;   /**< Immutable lifecycle implementation. */
    pf_storage_bus_mode_t mode;       /**< Selects the typed transfer interface. */
} pf_storage_bus_t;

/** Initialize a configured backend. Does not identify, mount, erase, or format media. */
pf_storage_bus_result_t pf_storage_bus_init(const pf_storage_bus_t *bus);
/** Release backend resources after the caller has completed device shutdown. */
void pf_storage_bus_deinit(const pf_storage_bus_t *bus);
/** Abort hardware only; interrupted device writes may already have taken effect. */
void pf_storage_bus_abort(const pf_storage_bus_t *bus);
/** Set a physical clock, reporting its calculated actual rate on success. */
pf_storage_bus_result_t pf_storage_bus_set_clock(const pf_storage_bus_t *bus,
                                                uint32_t requested_hz, uint32_t *actual_hz);
/** Execute a serial transaction; native SD requires its typed command/data API. */
pf_storage_bus_result_t pf_storage_bus_transfer(const pf_storage_bus_t *bus,
    const pf_storage_serial_transaction_t *transaction);
/** Validate the supported single/quad serial transaction shape before touching hardware. */
bool pf_storage_serial_transaction_valid(const pf_storage_serial_transaction_t *transaction);

#endif
