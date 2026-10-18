/** @file
 * @brief Static external NOR state, region initialization and serialized ownership gates.
 */
#include "nor_media.h"
#include "board/board_storage.h"
#include "drivers/nor_flash/nor.h"
#include "middleware/storage/levelx_block_device.h"
#include "middleware/storage/fatfs_volume.h"
#include "middleware/logging/logging.h"
#include "FreeRTOS.h"
#include "semphr.h"

static StaticSemaphore_t mutex_storage; /**< Application-lifetime recursive reservation storage. */
static SemaphoreHandle_t mutex; /**< Created before task startup. */
static pf_storage_bus_t bus; /**< Board-owned external transport handle. */
static pf_nor_t flash; /**< External flash only; never the Pico boot flash. */
static pf_levelx_nor_t volume; /**< Caller-selected LevelX region and fault diagnostics. */
static pf_block_device_t raw_device; /**< Non-owner-gated LevelX descriptor, accessed under mutex. */
static bool prepared; /**< Board pins have been prepared for this application. */
static bool usb_owner; /**< Exclusive owner; transitions and I/O hold mutex. */
static bool remount_requested; /**< Published on USB release and consumed by the storage worker. */
static bool firmware_context = false, usb_context = true; /**< Static descriptor owner identities. */

bool pf_nor_media_setup(void)
{
    if (mutex != NULL) {
        return true;
    }
    mutex = xSemaphoreCreateRecursiveMutexStatic(&mutex_storage);
    return mutex != NULL;
}

void pf_nor_media_lock(void)
{
    BaseType_t result = xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    configASSERT(result == pdTRUE);
    (void)result;
}

bool pf_nor_media_try_lock(void)
{
    return xSemaphoreTakeRecursive(mutex, 0u) == pdTRUE;
}

void pf_nor_media_unlock(void)
{
    BaseType_t result = xSemaphoreGiveRecursive(mutex);
    configASSERT(result == pdTRUE);
    (void)result;
}

bool pf_nor_media_ready(void)
{
    pf_nor_media_lock();
    bool result = volume.opened && !volume.faulted && flash.initialized;
    pf_nor_media_unlock();
    return result;
}

uint32_t pf_nor_media_sector_count(void)
{
    pf_nor_media_lock();
    uint32_t result = pf_nor_media_ready() ? volume.sector_count : 0u;
    pf_nor_media_unlock();
    return result;
}

pf_block_result_t pf_nor_media_initialize(void)
{
    pf_nor_media_lock();
    if (usb_owner || pf_fatfs_volume_is_mounted()) {
        pf_nor_media_unlock();
        return PF_BLOCK_NOT_READY;
    }
    if (pf_nor_media_ready()) {
        pf_nor_media_unlock();
        return PF_BLOCK_OK;
    }
    if (volume.opened && pf_levelx_nor_close(&volume) != LX_SUCCESS) {
        PF_LOG_ERROR("nor", "Failed to close faulted LevelX volume");
        pf_nor_media_unlock();
        return PF_BLOCK_IO_ERROR;
    }
    pf_storage_bus_deinit(&bus);
    if (!prepared) {
        if (pf_board_storage_prepare(&bus) != PF_STORAGE_BUS_OK) {
            PF_LOG_ERROR("nor", "External NOR board preparation failed");
            pf_nor_media_unlock();
            return PF_BLOCK_NOT_READY;
        }
        prepared = true;
    }
    pf_nor_transport_t transport;
    pf_nor_result_t result = pf_nor_transport_init(&bus, &transport);
    if (result == PF_NOR_OK) {
        result = pf_nor_init(&flash, &transport);
    }
#if PF_NOR_QSPI
    if (result == PF_NOR_OK) {
        result = pf_nor_enable_quad(&flash);
    }
#endif
    if (result != PF_NOR_OK) {
        PF_LOG_ERROR("nor", "External NOR initialization failed: %u", (unsigned)result);
        pf_storage_bus_deinit(&bus);
        flash.initialized = false;
        pf_nor_media_unlock();
        return PF_BLOCK_IO_ERROR;
    }
    UINT status = pf_levelx_nor_open(&volume, &flash, PF_NOR_REGION_OFFSET, PF_NOR_REGION_SIZE);
    if (status != LX_SUCCESS) {
        PF_LOG_ERROR("nor", "LevelX open failed: %u raw=%u system=%u", (unsigned)status,
                     (unsigned)volume.last_raw_result, (unsigned)volume.last_system_error);
        pf_nor_media_unlock();
        return PF_BLOCK_IO_ERROR;
    }
    raw_device = pf_levelx_block_device(&volume);
    pf_nor_media_unlock();
    return PF_BLOCK_OK;
}

static pf_block_result_t geometry(void *context, pf_block_geometry_t *value)
{
    (void)context;
    pf_nor_media_lock();
    pf_block_result_t result = pf_nor_media_ready() ? pf_block_geometry(&raw_device, value) : PF_BLOCK_NOT_READY;
    pf_nor_media_unlock();
    return result;
}

/** @brief Dispatch once under the shared reservation; ownership cannot change during a batch. */
static pf_block_result_t read_blocks(void *context, uint32_t first, void *buffer, uint32_t count)
{
    pf_nor_media_lock();
    pf_block_result_t result = usb_owner == *(const bool *)context && pf_nor_media_ready()
        ? pf_block_read(&raw_device, first, buffer, count) : PF_BLOCK_NOT_READY;
    pf_nor_media_unlock();
    return result;
}

static pf_block_result_t write_blocks(void *context, uint32_t first, const void *buffer, uint32_t count)
{
    pf_nor_media_lock();
    pf_block_result_t result = usb_owner == *(const bool *)context && pf_nor_media_ready()
        ? pf_block_write(&raw_device, first, buffer, count) : PF_BLOCK_NOT_READY;
    pf_nor_media_unlock();
    return result;
}

static pf_block_result_t sync_blocks(void *context)
{
    pf_nor_media_lock();
    pf_block_result_t result = usb_owner == *(const bool *)context && pf_nor_media_ready()
        ? pf_block_sync(&raw_device) : PF_BLOCK_NOT_READY;
    pf_nor_media_unlock();
    return result;
}

/** @brief Select a static descriptor whose context identifies the permitted owner. */
static const pf_block_device_t *device_for_owner(bool usb)
{
    static const pf_block_device_ops_t ops = {
        .geometry = geometry, .read = read_blocks, .write = write_blocks, .sync = sync_blocks,
    };
    static const pf_block_device_t firmware = {.ops = &ops, .context = &firmware_context};
    static const pf_block_device_t host = {.ops = &ops, .context = &usb_context};
    return usb ? &host : &firmware;
}

const pf_block_device_t *pf_nor_media_firmware_device(void)
{
    return device_for_owner(false);
}

const pf_block_device_t *pf_nor_media_usb_device(void)
{
    return device_for_owner(true);
}

pf_block_result_t pf_nor_media_acquire_usb(void)
{
    pf_nor_media_lock();
    pf_block_result_t result = PF_BLOCK_OK;
    if (!pf_nor_media_ready() || pf_fatfs_volume_is_mounted()) {
        result = PF_BLOCK_NOT_READY;
    } else {
        usb_owner = true;
    }
    pf_nor_media_unlock();
    return result;
}

pf_block_result_t pf_nor_media_release_usb(void)
{
    pf_nor_media_lock();
    if (usb_owner) {
        remount_requested = true;
        usb_owner = false;
    }
    pf_nor_media_unlock();
    return PF_BLOCK_OK;
}

pf_block_result_t pf_nor_media_release_usb_for_recovery(void)
{
    pf_nor_media_lock();
    pf_block_result_t result = !pf_nor_media_ready() && usb_owner
        ? pf_nor_media_release_usb() : PF_BLOCK_NOT_READY;
    pf_nor_media_unlock();
    return result;
}

bool pf_nor_media_usb_owned(void)
{
    pf_nor_media_lock();
    bool result = usb_owner;
    pf_nor_media_unlock();
    return result;
}

bool pf_nor_media_take_remount_request(void)
{
    pf_nor_media_lock();
    bool result = remount_requested;
    remount_requested = false;
    pf_nor_media_unlock();
    return result;
}
