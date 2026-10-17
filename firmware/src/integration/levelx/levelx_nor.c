#include "levelx_nor.h"

#include <string.h>

_Static_assert(sizeof(ULONG) == 4u, "LevelX NOR requires a 32-bit flash word");
_Static_assert(LX_NOR_SECTOR_SIZE * sizeof(ULONG) == 512u, "Expected 512-byte sectors");
#ifdef LX_DIRECT_READ
#error "Serial NOR must use LevelX driver callbacks, not direct memory reads"
#endif

/* LevelX uses pointer arithmetic for non-memory-mapped flash. This address is
 * an opaque word-address token, never dereferenced (LX_DIRECT_READ is disabled). */
#define ADDRESS_BASE ((uintptr_t)0x10000000u)

static bool initialized;

static pf_levelx_nor_t *volume_for(LX_NOR_FLASH *control)
{
    return control->lx_nor_flash_driver_info_ptr;
}

static UINT raw_result(pf_levelx_nor_t *volume, pf_nor_result_t result)
{
    if (!volume->faulted) {
        volume->last_raw_result = result;
    }
    if (result != PF_NOR_OK) {
        volume->faulted = true;
        return LX_ERROR;
    }
    return LX_SUCCESS;
}

static bool address_valid(pf_levelx_nor_t *volume, ULONG *address, ULONG words, uint32_t *offset)
{
    uintptr_t token = (uintptr_t)address;
    if (token < ADDRESS_BASE || token - ADDRESS_BASE > volume->size ||
        (token - ADDRESS_BASE) % sizeof(ULONG) != 0u ||
        words > (volume->size - (token - ADDRESS_BASE)) / sizeof(ULONG)) {
        volume->faulted = true;
        volume->last_raw_result = PF_NOR_INVALID_ARGUMENT;
        return false;
    }
    *offset = volume->offset + (uint32_t)(token - ADDRESS_BASE);
    return true;
}

static UINT read_words(LX_NOR_FLASH *control, ULONG *address, ULONG *destination, ULONG words)
{
    pf_levelx_nor_t *volume = volume_for(control);
    uint32_t offset;
    if (destination == NULL || words == 0u || !address_valid(volume, address, words, &offset)) {
        return LX_ERROR;
    }
    return raw_result(volume, pf_nor_read(volume->flash, offset, destination, (size_t)words * 4u));
}

static UINT write_words(LX_NOR_FLASH *control, ULONG *address, ULONG *source, ULONG words)
{
    pf_levelx_nor_t *volume = volume_for(control);
    uint32_t offset;
    if (source == NULL || words == 0u || !address_valid(volume, address, words, &offset)) {
        return LX_ERROR;
    }
    return raw_result(volume, pf_nor_program(volume->flash, offset, source, (size_t)words * 4u));
}

static UINT erase_block(LX_NOR_FLASH *control, ULONG block, ULONG erase_count)
{
    pf_levelx_nor_t *volume = volume_for(control);
    (void)erase_count;
    if (block >= volume->size / 4096u) {
        return raw_result(volume, PF_NOR_INVALID_ARGUMENT);
    }
    return raw_result(volume, pf_nor_erase(volume->flash, volume->offset + block * 4096u));
}

static UINT verify_block(LX_NOR_FLASH *control, ULONG block)
{
    pf_levelx_nor_t *volume = volume_for(control);
    ULONG words[32];
    if (block >= volume->size / 4096u) {
        return raw_result(volume, PF_NOR_INVALID_ARGUMENT);
    }
    for (uint32_t offset = 0u; offset < 4096u; offset += sizeof(words)) {
        UINT result = raw_result(volume, pf_nor_read(volume->flash,
            volume->offset + block * 4096u + offset, words, sizeof(words)));
        if (result != LX_SUCCESS) {
            return result;
        }
        for (size_t index = 0u; index < sizeof(words) / sizeof(words[0]); index++) {
            if (words[index] != UINT32_MAX) {
                return LX_ERROR; /* Expected non-erased result, not a media fault. */
            }
        }
    }
    return LX_SUCCESS;
}

static UINT system_error(LX_NOR_FLASH *control, UINT error)
{
    pf_levelx_nor_t *volume = volume_for(control);
    volume->last_system_error = error;
    volume->faulted = true;
    return LX_ERROR;
}

static UINT driver_init(LX_NOR_FLASH *control)
{
    pf_levelx_nor_t *volume = volume_for(control);
    control->lx_nor_flash_base_address = (ULONG *)ADDRESS_BASE;
    control->lx_nor_flash_total_blocks = volume->size / 4096u;
    control->lx_nor_flash_words_per_block = 4096u / sizeof(ULONG);
    control->lx_nor_flash_sector_buffer = volume->buffer;
    control->lx_nor_flash_driver_read = read_words;
    control->lx_nor_flash_driver_write = write_words;
    control->lx_nor_flash_driver_block_erase = erase_block;
    control->lx_nor_flash_driver_block_erased_verify = verify_block;
    control->lx_nor_flash_driver_system_error = system_error;
    return LX_SUCCESS;
}

UINT pf_levelx_nor_open(pf_levelx_nor_t *volume, pf_nor_t *flash,
                        uint32_t offset, uint32_t size)
{
    if (volume == NULL || volume->opened || flash == NULL || !flash->initialized ||
        flash->erase_size != 4096u || flash->page_size != 256u ||
        offset % 4096u != 0u || size % 4096u != 0u || size < 4u * 4096u ||
        size > UINT32_MAX - ADDRESS_BASE ||
        offset > flash->capacity_bytes || size > flash->capacity_bytes - offset) {
        return LX_ERROR;
    }
    if (!initialized) {
        lx_nor_flash_initialize();
        initialized = true;
    }
    memset(volume, 0, sizeof(*volume));
    volume->flash = flash;
    volume->offset = offset;
    volume->size = size;
    UINT result = lx_nor_flash_open_extended(&volume->control, "PicoForge NOR", driver_init, volume);
    if (result == LX_SUCCESS) {
        volume->opened = true;
        volume->sector_count = (size / 4096u - 2u) *
                              volume->control.lx_nor_flash_physical_sectors_per_block;
    }
    return result;
}

UINT pf_levelx_nor_read(pf_levelx_nor_t *volume, uint32_t sector, void *data)
{
    if (volume == NULL || !volume->opened || volume->faulted ||
        data == NULL || sector >= volume->sector_count) {
        return LX_ERROR;
    }
    ULONG words[LX_NOR_SECTOR_SIZE];
    UINT result = lx_nor_flash_sector_read(&volume->control, sector, words);
    if (result == LX_SUCCESS) {
        memcpy(data, words, sizeof(words));
    }
    return result;
}

UINT pf_levelx_nor_write(pf_levelx_nor_t *volume, uint32_t sector, const void *data)
{
    if (volume == NULL || !volume->opened || volume->faulted ||
        data == NULL || sector >= volume->sector_count) {
        return LX_ERROR;
    }
    ULONG words[LX_NOR_SECTOR_SIZE];
    memcpy(words, data, sizeof(words));
    return lx_nor_flash_sector_write(&volume->control, sector, words);
}

UINT pf_levelx_nor_sync(pf_levelx_nor_t *volume)
{
    if (volume == NULL || !volume->opened || volume->faulted) {
        return LX_ERROR;
    }
    return raw_result(volume, pf_nor_sync(volume->flash));
}

UINT pf_levelx_nor_close(pf_levelx_nor_t *volume)
{
    if (volume == NULL || !volume->opened) {
        return LX_ERROR;
    }
    UINT result = lx_nor_flash_close(&volume->control);
    if (result == LX_SUCCESS) {
        volume->opened = false;
    }
    return result;
}
