/** @file
 * @brief Synchronous logical sectors, independent of filesystem and physical transport.
 */
#ifndef PF_BLOCK_DEVICE_H
#define PF_BLOCK_DEVICE_H

#include <stdbool.h>
#include <stdint.h>

/** Logical block-operation outcome; backends retain detailed device diagnostics. */
typedef enum {
    PF_BLOCK_OK,               /**< Operation completed. */
    PF_BLOCK_INVALID_ARGUMENT, /**< Invalid descriptor, buffer, or sector range. */
    PF_BLOCK_NOT_READY,        /**< Media unavailable or access not permitted. */
    PF_BLOCK_WRITE_PROTECTED,  /**< Media cannot be written. */
    PF_BLOCK_IO_ERROR,         /**< Device operation failed; writes must not be replayed. */
} pf_block_result_t;

/** Current media metadata; availability does not grant I/O access. Query again after media/ownership changes. */
typedef struct {
    uint32_t sector_count;       /**< Number of addressable logical sectors. */
    uint16_t sector_size;        /**< Bytes per logical sector. */
    uint32_t erase_sector_count; /**< Erase granularity in sectors; 1 if unspecified. */
    bool write_protected;       /**< Writes must be rejected while true. */
} pf_block_geometry_t;

/** Backend operations; implementation owns serialization and access control.
 * Success means the entire requested batch completed. Recheck media and access at operation time:
 * a preceding geometry snapshot is not a reservation. No callback may retain the caller's buffer.
 */
typedef struct {
    pf_block_result_t (*geometry)(void *context, pf_block_geometry_t *geometry); /**< Query available media metadata. */
    pf_block_result_t (*read)(void *context, uint32_t first, void *buffer, uint32_t count); /**< Read sectors. */
    pf_block_result_t (*write)(void *context, uint32_t first, const void *buffer, uint32_t count); /**< Write sectors. */
    pf_block_result_t (*sync)(void *context); /**< Complete pending writes; never just discard them. */
} pf_block_device_ops_t;

/** Borrowed descriptor; operations and context outlive all users. NULL context is valid for singleton backends. */
typedef struct {
    const pf_block_device_ops_t *ops; /**< Backend operation table. */
    void *context;                   /**< Backend-owned state, possibly NULL. */
} pf_block_device_t;

/** Query media metadata; reject incomplete descriptors and invalid geometry.
 * Success does not reserve media or guarantee read/write/sync access under the current owner.
 */
pf_block_result_t pf_block_geometry(const pf_block_device_t *device, pf_block_geometry_t *geometry);
/** Read a nonempty in-range sector batch into a caller-owned buffer of sufficient size. */
pf_block_result_t pf_block_read(const pf_block_device_t *device, uint32_t first, void *buffer, uint32_t count);
/** Write a nonempty in-range sector batch; failure never authorizes automatic replay. */
pf_block_result_t pf_block_write(const pf_block_device_t *device, uint32_t first, const void *buffer, uint32_t count);
/** Complete pending writes under the backend's durability contract; no ownership change or media initialization. */
pf_block_result_t pf_block_sync(const pf_block_device_t *device);

#endif
