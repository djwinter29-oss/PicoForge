#ifndef PF_SD_CARD_BLOCK_H
#define PF_SD_CARD_BLOCK_H

#include <stdbool.h>
#include <stdint.h>

#define PF_SD_CARD_BLOCK_SIZE 512u

typedef enum {
    PF_SD_CARD_OK = 0,
    PF_SD_CARD_INVALID_ARGUMENT,
    PF_SD_CARD_CONFIG_LOCKED,
    PF_SD_CARD_NOT_CONFIGURED,
    PF_SD_CARD_INIT_FAILED,
    PF_SD_CARD_NOT_READY,
    PF_SD_CARD_IO_ERROR,
} pf_sd_card_result_t;

typedef enum {
    PF_SD_CARD_OWNER_FIRMWARE = 0,
    PF_SD_CARD_OWNER_USB_MSC,
} pf_sd_card_owner_t;

typedef struct {
    bool initialized;
    bool presence_known;
    bool present;
    bool cid_valid;
    uint32_t bus_clock_hz;
    uint32_t block_count;
    uint8_t cid[16];
} pf_sd_card_info_t;

/** Return whether the selected card transport has initialized successfully. */
bool pf_sd_card_is_initialized(void);

/** Return the initialized card's number of 512-byte sectors, or zero if unavailable. */
uint32_t pf_sd_card_block_count(void);

/** Read 512-byte sectors into a caller-owned buffer. */
pf_sd_card_result_t pf_sd_card_read_blocks(uint32_t first_block, void *buffer,
                                           uint32_t block_count);

/** Write 512-byte sectors from a caller-owned buffer. */
pf_sd_card_result_t pf_sd_card_write_blocks(uint32_t first_block, const void *buffer,
                                            uint32_t block_count);

/** Complete any pending write. */
pf_sd_card_result_t pf_sd_card_sync(void);

/** Select the only block-I/O client allowed to access the card. */
pf_sd_card_result_t pf_sd_card_set_owner(pf_sd_card_owner_t owner);

/** Read blocks on behalf of the USB MSC host while it owns the card. */
pf_sd_card_result_t pf_sd_card_msc_read_blocks(uint32_t first_block, void *buffer,
                                               uint32_t block_count);

/** Write blocks on behalf of the USB MSC host while it owns the card. */
pf_sd_card_result_t pf_sd_card_msc_write_blocks(uint32_t first_block, const void *buffer,
                                                uint32_t block_count);

/** Flush writes on behalf of the USB MSC host while it owns the card. */
pf_sd_card_result_t pf_sd_card_msc_sync(void);

/** Snapshot initialized card telemetry; presence is unknown until detect or init succeeds. */
void pf_sd_card_get_info(pf_sd_card_info_t *info);

/**
 * Set a callback run between polls of card waits that can last milliseconds
 * (write busy, command-line busy). Keep it short, such as taskYIELD(); NULL selects
 * plain busy polling. Set it before the first transfer; it is not synchronized.
 */
void pf_sd_card_set_idle_hook(void (*hook)(void));

#endif