#ifndef PF_STORAGE_TRANSPORT_H
#define PF_STORAGE_TRANSPORT_H

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

/** Check RP2040/RP2350 SPI pin roles; board/backend callers separately validate GPIO bounds and conflicts. */
bool pf_storage_spi_pins_match_controller(uint32_t spi_index, uint32_t clock_gpio,
                                         uint32_t mosi_gpio, uint32_t miso_gpio);

/** Immutable native SD signal allocation, independent of PIO resources. */
typedef struct {
    uint32_t clock_gpio;   /**< SD clock output. */
    uint32_t command_gpio; /**< Bidirectional command signal. */
    uint32_t data_gpio[4]; /**< DAT0-DAT3 in logical order; ascending, consecutive GPIOs. */
} pf_storage_sdio_pins_t;

/** Immutable single/quad serial signal allocation, independent of PIO resources. */
typedef struct {
    uint32_t clock_gpio;   /**< Serial clock output. */
    uint32_t data_gpio[4]; /**< IO0-IO3 in logical order; ascending, consecutive GPIOs. */
    uint32_t cs_gpio;      /**< Active-low device select. */
} pf_storage_qspi_pins_t;

/** Physical signaling mode, not the attached device's command protocol. */
typedef enum {
    PF_STORAGE_TRANSPORT_SPI,      /**< Clocked full-duplex single-lane SPI. */
    PF_STORAGE_TRANSPORT_NATIVE_SD, /**< Native SD command/data signaling, independent of implementation. */
    PF_STORAGE_TRANSPORT_QSPI,      /**< Single/quad serial signaling, independent of implementation. */
} pf_storage_transport_mode_t;

/** Transport outcome; device protocol layers translate into their public results. */
typedef enum {
    PF_STORAGE_TRANSPORT_OK,               /**< Requested wire operation completed. */
    PF_STORAGE_TRANSPORT_INVALID_ARGUMENT, /**< Invalid handle, configuration, or transfer shape. */
    PF_STORAGE_TRANSPORT_NOT_READY,        /**< Backend has not acquired its hardware resources. */
    PF_STORAGE_TRANSPORT_UNSUPPORTED,      /**< Backend cannot perform this operation. */
    PF_STORAGE_TRANSPORT_IO_ERROR,         /**< Hardware transfer failed. */
    PF_STORAGE_TRANSPORT_TIMEOUT,          /**< A bounded hardware wait expired. */
} pf_storage_transport_result_t;

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

/** Native wire clock ownership, independent of any PIO implementation. */
typedef enum {
    PF_STORAGE_SDIO_CLOCK_UNOWNED,        /**< Clock stopped, lines released. */
    PF_STORAGE_SDIO_CLOCK_COMMAND,        /**< Command signaling owns the clock. */
    PF_STORAGE_SDIO_CLOCK_INITIALIZATION, /**< Released-line startup clocks. */
    PF_STORAGE_SDIO_CLOCK_DATA_TX,        /**< Data transmission owns the clock. */
} pf_storage_sdio_clock_owner_t;

/** Physical failures; device response and CRC interpretation remains above this layer. */
typedef enum {
    PF_STORAGE_SDIO_ERROR_NONE,         /**< No physical failure. */
    PF_STORAGE_SDIO_ERROR_STATE,        /**< Invalid or unavailable wire state. */
    PF_STORAGE_SDIO_ERROR_PIO_TIMEOUT,  /**< Wire engine completion deadline expired. */
    PF_STORAGE_SDIO_ERROR_DMA_TIMEOUT,  /**< Memory transfer deadline expired. */
    PF_STORAGE_SDIO_ERROR_BUSY_TIMEOUT, /**< Data line remained busy. */
    PF_STORAGE_SDIO_ERROR_TX_STALL,     /**< Transmit starvation. */
    PF_STORAGE_SDIO_ERROR_RX_STALL,     /**< Receive overflow. */
} pf_storage_sdio_error_t;

/** Wire-operation domain for device diagnostics. */
typedef enum {
    PF_STORAGE_SDIO_PHASE_COMMAND, /**< Command and response. */
    PF_STORAGE_SDIO_PHASE_DATA,    /**< Payload transfer. */
    PF_STORAGE_SDIO_PHASE_BUSY,    /**< Data-line release. */
    PF_STORAGE_SDIO_PHASE_SETUP,   /**< Resource or clock setup. */
} pf_storage_sdio_phase_t;

/** Borrowed diagnostic callback; invoked synchronously during wire operations. */
typedef void (*pf_storage_sdio_error_hook_t)(void *context, pf_storage_sdio_error_t error,
                                           pf_storage_sdio_phase_t phase);

/** Read-only native wire snapshot, not a mutable backend context. */
typedef struct {
    bool ready;                     /**< Backend resources have been acquired. */
    bool rx_ready;                  /**< Raw reception is available. */
    bool mode_restore_failed;       /**< Command signaling could not be restored. */
    bool data_ready;                /**< DAT0 is currently high. */
    uint32_t current_clock_hz;       /**< Calculated actual wire clock. */
    uint32_t maximum_clock_hz;       /**< Backend's current representable ceiling. */
    pf_storage_sdio_clock_owner_t clock_owner; /**< Current wire-clock owner. */
    uint32_t last_response_words[5]; /**< Raw response, including partial failures. */
    size_t last_response_word_count; /**< Valid words in the raw response. */
} pf_storage_native_state_t;

/** Native backend operations; buffers remain borrowed until finish/cleanup.
 * No card commands, CRC policy, or retries are generated by these operations.
 */
typedef struct {
    pf_storage_native_state_t (*state)(void *context); /**< Return a wire snapshot. */
    void (*hooks)(void *context, pf_storage_sdio_error_hook_t error, void *error_context,
                  void (*idle)(void)); /**< Install synchronous borrowed callbacks. */
    bool (*prepare)(void *context); /**< Reset and emit 80 released-line clocks. */
    bool (*restore)(void *context); /**< Restore command signaling after TX. */
    bool (*command)(void *context, const uint8_t frame[6], uint16_t bits,
                    uint32_t *words, size_t capacity); /**< Transfer a framed command and raw response. */
    bool (*rx_start)(void *context, uint32_t *buffer, uint32_t words,
                     uint32_t nibbles, uint32_t blocks); /**< Arm raw RX before the card command. */
    bool (*rx_finish)(void *context, uint32_t timeout_us); /**< Wait for RX with a finite deadline. */
    void (*rx_cleanup)(void *context); /**< Stop RX and clear completion state. */
    bool (*rx_poll)(void *context, uint32_t start, uint32_t timeout_us,
                    bool *complete); /**< Poll completion, deadline and stalls. */
    void (*rx_wait)(void *context); /**< Perform one bounded platform wait. */
    bool (*rx_progress)(void *context, const uint32_t *buffer, uint32_t capacity,
                        uint32_t *words); /**< Report memory-committed words with an acquire barrier. */
    bool (*tx_prepare)(void *context); /**< Load TX resources retained across payloads. */
    bool (*tx_start)(void *context, const uint32_t *buffer, uint32_t words,
                     uint32_t nibbles); /**< Start framed raw TX. */
    bool (*tx_finish)(void *context, uint32_t timeout_us,
                      uint32_t *response); /**< Wait for TX and return the raw response token. */
    bool (*wait_ready)(void *context, uint32_t timeout_us); /**< Bounded DAT0-release wait. */
} pf_storage_native_ops_t;

/** Backend lifecycle; operations are synchronous and caller-serialized. */
typedef struct {
    pf_storage_transport_result_t (*init)(void *context); /**< Acquire resources and set idle pin levels. */
    void (*deinit)(void *context);                 /**< Release resources; never sync the device. */
    void (*abort)(void *context);                  /**< Stop hardware without device commands. */
    pf_storage_transport_result_t (*set_clock)(void *context, uint32_t requested_hz,
                                        uint32_t *actual_hz); /**< Program and report actual frequency. */
    pf_storage_transport_result_t (*transfer)(void *context,
        const pf_storage_serial_transaction_t *transaction); /**< NULL for native SD. */
    pf_storage_transport_result_t (*select)(void *context, bool selected); /**< Raw serial CS control. */
    pf_storage_transport_result_t (*exchange)(void *context, const uint8_t *tx,
        uint8_t *rx, size_t length); /**< Raw serial bytes; NULL TX sends 0xff. */
    const pf_storage_native_ops_t *native; /**< NULL without native wire support. */
    uint64_t (*time_us)(void); /**< Monotonic wrapping platform microseconds. */
    void (*delay_ms)(uint32_t duration); /**< Bounded platform delay. */
    bool (*enable_wait)(void *context); /**< Enable optional bounded scheduler waits. */
} pf_storage_transport_ops_t;

/** Non-owning handle. Context and operations must outlive every synchronous call. */
typedef struct {
    void *context;                    /**< Backend-owned configuration and hardware state. */
    const pf_storage_transport_ops_t *ops;   /**< Immutable lifecycle implementation. */
    pf_storage_transport_mode_t mode;       /**< Selects the typed transfer interface. */
    bool (*present)(const void *context); /**< Optional board-owned media-presence callback. */
    const void *presence_context;     /**< Borrowed immutable presence callback context. */
} pf_storage_transport_t;

/** Initialize a configured backend. Does not identify, mount, erase, or format media. */
pf_storage_transport_result_t pf_storage_transport_init(const pf_storage_transport_t *bus);
/** Release backend resources after the caller has completed device shutdown. */
void pf_storage_transport_deinit(const pf_storage_transport_t *bus);
/** Abort hardware only; interrupted device writes may already have taken effect. */
void pf_storage_transport_abort(const pf_storage_transport_t *bus);
/** Set a physical clock, reporting its calculated actual rate on success. */
pf_storage_transport_result_t pf_storage_transport_set_clock(const pf_storage_transport_t *bus,
                                                uint32_t requested_hz, uint32_t *actual_hz);
/** Execute a serial transaction; native SD requires its typed command/data API. */
pf_storage_transport_result_t pf_storage_transport_transfer(const pf_storage_transport_t *bus,
    const pf_storage_serial_transaction_t *transaction);
/** Validate the supported single/quad serial transaction shape before touching hardware. */
bool pf_storage_serial_transaction_valid(const pf_storage_serial_transaction_t *transaction);

/** Validate lifecycle and the selected wire capability without touching hardware. */
bool pf_storage_transport_valid(const pf_storage_transport_t *transport, pf_storage_transport_mode_t mode);
/** Validate serial transaction support and report quad capability without inspecting device protocol. */
pf_storage_transport_result_t pf_storage_transport_validate_serial(const pf_storage_transport_t *transport,
    bool *quad_supported);
/** Compare immutable descriptors; neither context is inspected or owned. */
bool pf_storage_transport_same(const pf_storage_transport_t *left, const pf_storage_transport_t *right);
/** Return whether a board presence detector is installed. */
bool pf_storage_transport_presence_known(const pf_storage_transport_t *transport);
/** Sample board presence; without a detector, media is assumed present. */
bool pf_storage_transport_present(const pf_storage_transport_t *transport);
/** Read platform microseconds; requires a validated descriptor with a time callback. */
uint64_t pf_storage_transport_time_us(const pf_storage_transport_t *transport);
/** Delay for platform milliseconds; requires a validated descriptor with a delay callback. */
void pf_storage_transport_delay_ms(const pf_storage_transport_t *transport, uint32_t duration);
/** Enable optional bounded waits; false means unavailable or invalid descriptor. */
bool pf_storage_transport_enable_wait(const pf_storage_transport_t *transport);
/** Assert/deassert serial CS; unsupported operations return an explicit result. */
pf_storage_transport_result_t pf_storage_transport_select(const pf_storage_transport_t *transport, bool selected);
/** Exchange serial bytes without changing CS; NULL TX sends 0xff, NULL RX discards. */
pf_storage_transport_result_t pf_storage_transport_exchange(const pf_storage_transport_t *transport,
    const uint8_t *tx, uint8_t *rx, size_t length);
/** Snapshot native state; an unavailable native descriptor reports ready=false. */
pf_storage_native_state_t pf_storage_transport_native_state(const pf_storage_transport_t *transport);
/** Install native diagnostic and idle callbacks, borrowed until replaced or deinitialized. */
bool pf_storage_transport_native_hooks(const pf_storage_transport_t *transport,
    pf_storage_sdio_error_hook_t error, void *context, void (*idle)(void));
/** Reset wire state and emit 80 released-line initialization clocks. */
bool pf_storage_transport_native_prepare(const pf_storage_transport_t *transport);
/** Restore command signaling without issuing card commands. */
bool pf_storage_transport_native_restore(const pf_storage_transport_t *transport);
/** Send a framed command; response is raw word packing, not parsed card status. */
bool pf_storage_transport_native_command(const pf_storage_transport_t *transport, const uint8_t frame[6],
    uint16_t bits, uint32_t *words, size_t capacity);
/** Arm raw RX before a device command; buffer remains borrowed until cleanup. */
bool pf_storage_transport_native_rx_start(const pf_storage_transport_t *transport, uint32_t *buffer,
    uint32_t words, uint32_t nibbles, uint32_t blocks);
/** Wait for raw RX with a finite timeout. */
bool pf_storage_transport_native_rx_finish(const pf_storage_transport_t *transport, uint32_t timeout_us);
/** Stop RX and clear its completion state. */
void pf_storage_transport_native_rx_cleanup(const pf_storage_transport_t *transport);
/** Poll RX completion and deadline; false reports an invalid operation or wire failure. */
bool pf_storage_transport_native_rx_poll(const pf_storage_transport_t *transport, uint32_t start,
    uint32_t timeout_us, bool *complete);
/** Execute one bounded scheduler wait, if supported. */
void pf_storage_transport_native_rx_wait(const pf_storage_transport_t *transport);
/** Report DMA-committed words with an acquire barrier before returning. */
bool pf_storage_transport_native_rx_progress(const pf_storage_transport_t *transport,
    const uint32_t *buffer, uint32_t capacity, uint32_t *words);
/** Load TX resources retained across payloads. */
bool pf_storage_transport_native_tx_prepare(const pf_storage_transport_t *transport);
/** Start raw framed TX; buffer remains borrowed until finish. */
bool pf_storage_transport_native_tx_start(const pf_storage_transport_t *transport,
    const uint32_t *buffer, uint32_t words, uint32_t nibbles);
/** Wait for TX and busy release; return the raw response token without interpretation. */
bool pf_storage_transport_native_tx_finish(const pf_storage_transport_t *transport,
    uint32_t timeout_us, uint32_t *response);
/** Wait for DAT0 high with a finite timeout. */
bool pf_storage_transport_native_wait_ready(const pf_storage_transport_t *transport, uint32_t timeout_us);

#endif
