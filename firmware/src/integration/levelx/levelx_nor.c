/** @file
 * @brief Bounded serial-NOR LevelX callbacks and caller-owned cache lifecycle.
 */
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

/** @brief Read bounded raw words; cache only wear-count words used by every reclaim scan. */
static UINT read_words(LX_NOR_FLASH *control, ULONG *address, ULONG *destination, ULONG words)
{
    pf_levelx_nor_t *volume = volume_for(control);
    uint32_t offset;
    if (destination == NULL || words == 0u || !address_valid(volume, address, words, &offset)) {
        return LX_ERROR;
    }
    uint32_t relative = offset - volume->offset;
    uint32_t block = relative / 4096u;
    uint32_t bit = 1u << (block % 32u);
    bool wear_count = words == 1u && relative % 4096u == 0u;
    if (wear_count && !volume->faulted && (volume->erase_count_valid[block / 32u] & bit) != 0u) {
        *destination = volume->erase_counts[block];
        return LX_SUCCESS;
    }
    UINT result = raw_result(volume, pf_nor_read(volume->flash, offset, destination, (size_t)words * 4u));
    if (result == LX_SUCCESS && wear_count) {
        volume->erase_counts[block] = *destination;
        volume->erase_count_valid[block / 32u] |= bit;
    }
    return result;
}

/** @brief Update cached wear counts only after the raw driver has verified the program. */
static UINT write_words(LX_NOR_FLASH *control, ULONG *address, ULONG *source, ULONG words)
{
    pf_levelx_nor_t *volume = volume_for(control);
    uint32_t offset;
    if (source == NULL || words == 0u || !address_valid(volume, address, words, &offset)) {
        return LX_ERROR;
    }
    UINT result = raw_result(volume, pf_nor_program(volume->flash, offset, source, (size_t)words * 4u));
    uint32_t relative = offset - volume->offset;
    uint32_t last = (relative + words * 4u - 1u) / 4096u;
    for (uint32_t block = relative / 4096u; block <= last; ++block) {
        if (block * 4096u < relative) {
            continue;
        }
        uint32_t bit = 1u << (block % 32u);
        volume->erase_count_valid[block / 32u] &= ~bit;
        if (result == LX_SUCCESS) {
            volume->erase_counts[block] = source[(block * 4096u - relative) / 4u];
            volume->erase_count_valid[block / 32u] |= bit;
        }
    }
    return result;
}

static UINT erase_block(LX_NOR_FLASH *control, ULONG block, ULONG erase_count)
{
    pf_levelx_nor_t *volume = volume_for(control);
    (void)erase_count;
    if (block >= volume->size / 4096u) {
        return raw_result(volume, PF_NOR_INVALID_ARGUMENT);
    }
    volume->erase_count_valid[block / 32u] &= ~(1u << (block % 32u));
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
        size / 4096u > PF_LEVELX_NOR_MAX_BLOCKS ||
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
        uint32_t bitmap_bytes = ((volume->control.lx_nor_flash_total_physical_sectors + 31u) / 32u) * 4u;
        uint32_t obsolete_bytes = ((size / 4096u + 3u) / 4u) * 4u;
        uint32_t cache_bytes = bitmap_bytes + obsolete_bytes;
        if (cache_bytes < LX_NOR_SECTOR_SIZE) {
            cache_bytes = LX_NOR_SECTOR_SIZE;
        }
        result = lx_nor_flash_extended_cache_enable(&volume->control, volume->cache, cache_bytes);
        if (result != LX_SUCCESS) {
            volume->faulted = true;
        }
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

UINT pf_levelx_nor_trim(pf_levelx_nor_t *volume, uint32_t sector)
{
    if (volume == NULL || !volume->opened || volume->faulted || sector >= volume->sector_count) {
        return LX_ERROR;
    }
    UINT result = lx_nor_flash_sector_release(&volume->control, sector);
    if (volume->faulted) {
        return LX_ERROR;
    }
    return result == LX_SECTOR_NOT_FOUND ? LX_SUCCESS : result;
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
