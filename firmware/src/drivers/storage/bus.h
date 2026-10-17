#ifndef PF_STORAGE_BUS_H
#define PF_STORAGE_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Immutable mode-0 SPI signal allocation; chip select is software-controlled. */
typedef struct {
    uint32_t clock_gpio; /**< Serial clock output. */
    uint32_t mosi_gpio;  /**< Controller transmit signal. */
    uint32_t miso_gpio;  /**< Controller receive signal. */
    uint32_t cs_gpio;    /**< Active-low device select. */
} pf_storage_spi_pins_t;

/** Check controller pin roles; board/backend callers separately validate bounds and conflicts. */
bool pf_storage_spi_pins_match_controller(uint32_t spi_index, uint32_t clock_gpio,
                                         uint32_t mosi_gpio, uint32_t miso_gpio);

/** Immutable native CMD/DAT signal allocation, independent of hardware resources. */
typedef struct {
    uint32_t clock_gpio;   /**< Clock output. */
    uint32_t command_gpio; /**< Bidirectional command signal. */
    uint32_t data_gpio[4]; /**< DAT0-DAT3 in logical order; ascending, consecutive GPIOs. */
} pf_storage_sdio_pins_t;

/** Immutable single/quad serial signal allocation. */
typedef struct {
    uint32_t clock_gpio;   /**< Serial clock output. */
    uint32_t data_gpio[4]; /**< IO0-IO3 in logical order; ascending, consecutive GPIOs. */
    uint32_t cs_gpio;      /**< Active-low device select. */
} pf_storage_qspi_pins_t;

/** Physical signaling mode, not an attached device's command protocol. */
typedef enum {
    PF_STORAGE_BUS_SPI,       /**< Full-duplex single-lane SPI. */
    PF_STORAGE_BUS_NATIVE_SD, /**< Separate bidirectional CMD and DAT signals. */
    PF_STORAGE_BUS_QSPI,      /**< Single/quad serial signaling. */
} pf_storage_bus_mode_t;

/** Physical outcome; device drivers translate into their public results. */
typedef enum {
    PF_STORAGE_BUS_OK,               /**< Requested operation completed. */
    PF_STORAGE_BUS_INVALID_ARGUMENT, /**< Invalid handle or transfer shape. */
    PF_STORAGE_BUS_NOT_READY,        /**< Resources unavailable. */
    PF_STORAGE_BUS_UNSUPPORTED,      /**< Capability unavailable. */
    PF_STORAGE_BUS_IO_ERROR,         /**< Hardware transfer failed. */
    PF_STORAGE_BUS_TIMEOUT,          /**< Bounded wait expired. */
} pf_storage_bus_result_t;

/** Raw serial capability, defined in serial.h. */
typedef struct pf_storage_serial_ops pf_storage_serial_ops_t;
/** SD CMD/DAT wire capability, defined in sd_wire.h. */
typedef struct pf_storage_sd_wire_ops pf_storage_sd_wire_ops_t;

/** Common lifecycle and platform services; calls are externally serialized. */
typedef struct {
    pf_storage_bus_result_t (*init)(void *context); /**< Acquire resources; no device commands. */
    void (*deinit)(void *context); /**< Release resources; never sync the device. */
    void (*abort)(void *context); /**< Stop hardware; never replay transfers. */
    pf_storage_bus_result_t (*set_clock)(void *context, uint32_t requested_hz,
                                        uint32_t *actual_hz); /**< Set and report calculated clock. */
    const pf_storage_serial_ops_t *serial; /**< Optional raw serial operations. */
    const pf_storage_sd_wire_ops_t *sd_wire; /**< Optional SD CMD/DAT wire operations. */
    uint64_t (*time_us)(void); /**< Wrapping monotonic platform microseconds. */
    void (*delay_ms)(uint32_t duration); /**< Platform millisecond delay. */
    bool (*enable_wait)(void *context); /**< Install optional bounded scheduler waits. */
} pf_storage_bus_ops_t;

/** Non-owning descriptor; context and operations must outlive all access. */
typedef struct {
    void *context; /**< Backend-owned configuration and hardware state. */
    const pf_storage_bus_ops_t *ops; /**< Immutable implementation. */
    pf_storage_bus_mode_t mode; /**< Physical wire interface. */
    bool (*present)(const void *context); /**< Optional board-owned presence detector. */
    const void *presence_context; /**< Borrowed immutable detector argument. */
} pf_storage_bus_t;

/** Acquire a configured backend without identifying or accessing media. */
pf_storage_bus_result_t pf_storage_bus_init(const pf_storage_bus_t *bus);
/** Release resources after the device owner completes protocol shutdown. */
void pf_storage_bus_deinit(const pf_storage_bus_t *bus);
/** Stop hardware; interrupted writes may already have taken effect. */
void pf_storage_bus_abort(const pf_storage_bus_t *bus);
/** Set wire clock, reporting the calculated actual rate on success. */
pf_storage_bus_result_t pf_storage_bus_set_clock(const pf_storage_bus_t *bus,
                                                uint32_t requested_hz, uint32_t *actual_hz);
/** Validate common lifecycle/services for a mode; wire capabilities validate their own operations. */
bool pf_storage_bus_valid(const pf_storage_bus_t *bus, pf_storage_bus_mode_t mode);
/** Compare immutable descriptors without inspecting backend state. */
bool pf_storage_bus_same(const pf_storage_bus_t *left, const pf_storage_bus_t *right);
/** Return whether a board presence detector is installed. */
bool pf_storage_bus_presence_known(const pf_storage_bus_t *bus);
/** Sample presence; absent a detector, assume media is present. */
bool pf_storage_bus_present(const pf_storage_bus_t *bus);
/** Read microseconds; requires a validated descriptor with a time callback. */
uint64_t pf_storage_bus_time_us(const pf_storage_bus_t *bus);
/** Delay milliseconds; requires a validated descriptor with a delay callback. */
void pf_storage_bus_delay_ms(const pf_storage_bus_t *bus, uint32_t duration);
/** Install optional bounded waits; false means unavailable or invalid. */
bool pf_storage_bus_enable_wait(const pf_storage_bus_t *bus);

#endif
