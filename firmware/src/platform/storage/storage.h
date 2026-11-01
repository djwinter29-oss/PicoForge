/** @file
 * @brief Storage service: the single platform boundary for the filesystem-backing medium.
 *
 * Layers above platform storage see storage only as a mountable filesystem that can be exported to a USB host.
 * The service owns media lifecycle, presence debounce, paced automount, recovery, and exclusive firmware/USB MSC
 * ownership. Backend types (SD today) never appear in this header.
 */
#ifndef PF_PLATFORM_STORAGE_H
#define PF_PLATFORM_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "platform/storage/block_device.h"

/** Physical medium behind the filesystem. */
typedef enum {
    PF_STORAGE_MEDIUM_NONE = 0, /**< No medium configured. */
    PF_STORAGE_MEDIUM_SDCARD,   /**< Removable SD card. */
} pf_storage_medium_t;

/** Physical bus to the medium. */
typedef enum {
    PF_STORAGE_BUS_UNKNOWN = 0, /**< Not configured. */
    PF_STORAGE_BUS_SDIO,        /**< Native SD bus. */
    PF_STORAGE_BUS_SPI,         /**< SPI bus. */
} pf_storage_bus_t;

/** Storage lifecycle state; runtime status fields are valid from ABSENT onward. */
typedef enum {
    PF_STORAGE_STATE_NOT_CONFIGURED = 0, /**< Board provides no usable medium. */
    PF_STORAGE_STATE_BUSY,               /**< Reservation held elsewhere; runtime fields not sampled. */
    PF_STORAGE_STATE_ABSENT,             /**< Removable medium confirmed absent. */
    PF_STORAGE_STATE_UNINITIALIZED,      /**< Present or unknown, but not usable yet or failed. */
    PF_STORAGE_STATE_READY,              /**< Usable by firmware; no filesystem mounted. */
    PF_STORAGE_STATE_MOUNTED,            /**< Filesystem mounted for firmware use. */
    PF_STORAGE_STATE_USB,                /**< Exported to the USB host; firmware I/O is rejected. */
} pf_storage_state_t;

/** Operation recorded with the last failure. */
typedef enum {
    PF_STORAGE_OPERATION_NONE = 0,    /**< No failure recorded. */
    PF_STORAGE_OPERATION_INIT,        /**< Medium initialization. */
    PF_STORAGE_OPERATION_READ,        /**< Owner-gated read. */
    PF_STORAGE_OPERATION_WRITE,       /**< Owner-gated write. */
    PF_STORAGE_OPERATION_SYNC,        /**< Owner-gated sync. */
    PF_STORAGE_OPERATION_MSC_ACQUIRE, /**< USB export. */
} pf_storage_operation_t;

/** Backend-neutral snapshot for telemetry and user status. */
typedef struct {
    pf_storage_medium_t medium; /**< Board medium; always valid. */
    pf_storage_bus_t bus; /**< Board bus; always valid. */
    uint8_t bus_gpios[6]; /**< Bus pins (SDIO: CLK, CMD, D0-D3; SPI: SCK, MOSI, MISO, CS); UINT8_MAX unused. */
    uint8_t detect_gpio; /**< Presence-detect pin; UINT8_MAX when none. */
    pf_storage_state_t state; /**< Lifecycle state. */
    bool presence_known; /**< Presence detection produced a sample. */
    bool high_speed; /**< Medium runs its faster timing mode. */
    bool id_valid; /**< id holds the medium identity (SD: raw CID). */
    uint8_t id[16]; /**< Raw medium identity bytes. */
    uint32_t bus_clock_hz; /**< Active bus clock; 0 when unknown. */
    uint32_t sector_count; /**< 512-byte sectors; 0 when unknown. */
    pf_storage_operation_t last_failure; /**< Operation of the last failure, NONE when none. */
    pf_block_result_t last_failure_result; /**< Result of the last failure. */
} pf_storage_status_t;

/** @brief Bind board configuration and the execution port before task startup; never touches the medium.
 * @return PF_BLOCK_OK, PF_BLOCK_NOT_READY when the board has no medium, or PF_BLOCK_IO_ERROR on port failure.
 */
pf_block_result_t pf_storage_setup(void);
/** @brief Initialize the medium for firmware use; rejected while exported or mounted. Never formats. */
pf_block_result_t pf_storage_initialize(void);
/** Borrow the static firmware-gated block descriptor for FatFs. */
const pf_block_device_t *pf_storage_firmware_device(void);
/** @brief Copy a status snapshot without blocking; board fields are always filled.
 * @param status Receives the snapshot; NULL is ignored.
 */
void pf_storage_get_status(pf_storage_status_t *status);

/** @brief Run one presence-debounce and automount step; call only from the sole FatFs executor.
 * Unmounts on confirmed removal or lost initialization, and retries failed mounts at a paced interval.
 */
void pf_storage_poll(void);
/** @brief Reinitialize media after a USB fault handoff without mounting; blocks automount until an explicit mount.
 * @return True when the medium reinitialized. Sole FatFs executor only.
 */
bool pf_storage_recover(void);
/** @brief Attempt one mount after a forced USB return; failure blocks automount until an explicit mount.
 * @return True when mounted. Sole FatFs executor only.
 */
bool pf_storage_return_from_usb(void);
/** @brief Unregister FatFs before USB export and pause automount. @return False keeps the mount and logs. */
bool pf_storage_unmount_for_usb(void);
/** @brief Update automount policy after a firmware command that may have mounted or unmounted the volume.
 * @param was_mounted Mount state sampled before the command; an explicit unmount suppresses automount.
 */
void pf_storage_note_command(bool was_mounted);
/** True only when presence is known absent and debounced removal has been confirmed. */
bool pf_storage_media_absent(void);

/** @name USB MSC export
 * Callers hold the storage reservation across ownership transitions; it is recursive.
 * @{
 */
/** Take the storage reservation, blocking. */
void pf_storage_lock(void);
/** Take the storage reservation without blocking. @return False when held elsewhere. */
bool pf_storage_try_lock(void);
/** Release one level of the storage reservation. */
void pf_storage_unlock(void);
/** True when the medium is initialized and can serve USB transfers. */
bool pf_storage_msc_ready(void);
/** Medium capacity in 512-byte sectors; 0 when not initialized. */
uint32_t pf_storage_msc_count(void);
/** Export initialized, unmounted media to USB. */
pf_block_result_t pf_storage_msc_acquire(void);
/** Return USB media to firmware after caller-checked eject/sync; does not flush. */
pf_block_result_t pf_storage_msc_release(void);
/** Return faulted USB media after host access is revoked; rejects healthy media and never claims a flush. */
pf_block_result_t pf_storage_msc_release_for_recovery(void);
/** Sync under USB ownership. */
pf_block_result_t pf_storage_msc_sync(void);
/** Read sectors under USB ownership, without retries. */
pf_block_result_t pf_storage_msc_read(uint32_t first, void *buffer, uint32_t count);
/** Write sectors under USB ownership, without retries. */
pf_block_result_t pf_storage_msc_write(uint32_t first, const void *buffer, uint32_t count);
/** @} */

#endif
