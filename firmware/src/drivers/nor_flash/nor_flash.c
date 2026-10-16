#include "nor_flash.h"

#include <string.h>

static pf_nor_result_t execute(pf_nor_flash_t *flash, uint8_t command,
                                uint32_t address, uint8_t address_bytes,
                                const uint8_t *tx, uint8_t *rx, size_t length,
                                uint8_t lanes, uint8_t dummy)
{
    const pf_nor_transaction_t transaction = {
        .command = command, .address = address, .address_bytes = address_bytes,
        .tx = tx, .rx = rx, .length = length, .data_lanes = lanes,
        .dummy_cycles = dummy,
    };
    return flash->transport.execute(flash->transport.context, &transaction);
}

static pf_nor_result_t status(pf_nor_flash_t *flash, uint8_t command, uint8_t *value)
{
    return execute(flash, command, 0u, 0u, NULL, value, 1u, 1u, 0u);
}

static pf_nor_result_t wait_ready(pf_nor_flash_t *flash, uint64_t timeout_us)
{
    uint64_t start = flash->transport.time_us(flash->transport.context);
    for (;;) {
        uint8_t value;
        pf_nor_result_t result = status(flash, 0x05u, &value);
        if (result != PF_NOR_OK) {
            return result;
        }
        if ((value & 1u) == 0u) {
            return PF_NOR_OK;
        }
        if (flash->transport.time_us(flash->transport.context) - start >= timeout_us) {
            return PF_NOR_TIMEOUT;
        }
    }
}

static pf_nor_result_t write_enable(pf_nor_flash_t *flash)
{
    pf_nor_result_t result = execute(flash, 0x06u, 0u, 0u, NULL, NULL, 0u, 1u, 0u);
    uint8_t value;
    if (result != PF_NOR_OK) {
        return result;
    }
    result = status(flash, 0x05u, &value);
    return result != PF_NOR_OK ? result : ((value & 2u) != 0u ? PF_NOR_OK : PF_NOR_VERIFY_ERROR);
}

static bool range_valid(const pf_nor_flash_t *flash, uint32_t address, size_t length)
{
    return address < flash->capacity_bytes && length <= flash->capacity_bytes - address;
}

static pf_nor_result_t verify(pf_nor_flash_t *flash, uint32_t address,
                               const uint8_t *expected, size_t length)
{
    uint8_t actual[64];
    while (length != 0u) {
        size_t chunk = length < sizeof(actual) ? length : sizeof(actual);
        pf_nor_result_t result = execute(flash, 0x03u, address, 3u, NULL, actual, chunk, 1u, 0u);
        if (result != PF_NOR_OK) {
            return result;
        }
        for (size_t index = 0u; index < chunk; index++) {
            if (actual[index] != (expected != NULL ? expected[index] : 0xffu)) {
                return PF_NOR_VERIFY_ERROR;
            }
        }
        address += (uint32_t)chunk;
        length -= chunk;
        if (expected != NULL) {
            expected += chunk;
        }
    }
    return PF_NOR_OK;
}

pf_nor_result_t pf_nor_flash_init(pf_nor_flash_t *flash, const pf_nor_transport_t *transport)
{
    uint8_t id[3];
    if (flash == NULL || transport == NULL || transport->execute == NULL || transport->time_us == NULL) {
        return PF_NOR_INVALID_ARGUMENT;
    }
    pf_nor_transport_t copy = *transport;
    memset(flash, 0, sizeof(*flash));
    flash->transport = copy;
    pf_nor_result_t result = wait_ready(flash, 3000000u);
    if (result != PF_NOR_OK) {
        return result;
    }
    result = execute(flash, 0x9fu, 0u, 0u, NULL, id, sizeof(id), 1u, 0u);
    if (result != PF_NOR_OK) {
        return result;
    }
    if (id[0] != 0xefu || id[1] != 0x40u || id[2] != 0x18u) {
        return PF_NOR_UNSUPPORTED;
    }
    flash->capacity_bytes = 16u * 1024u * 1024u;
    flash->page_size = 256u;
    flash->erase_size = 4096u;
    flash->initialized = true;
    return PF_NOR_OK;
}

pf_nor_result_t pf_nor_flash_sync(pf_nor_flash_t *flash)
{
    if (flash == NULL) {
        return PF_NOR_INVALID_ARGUMENT;
    }
    return flash->initialized ? wait_ready(flash, 3000000u) : PF_NOR_NOT_READY;
}

pf_nor_result_t pf_nor_flash_read(pf_nor_flash_t *flash, uint32_t address, void *buffer, size_t length)
{
    if (flash == NULL || buffer == NULL || length == 0u) {
        return PF_NOR_INVALID_ARGUMENT;
    }
    if (!flash->initialized) {
        return PF_NOR_NOT_READY;
    }
    if (!range_valid(flash, address, length)) {
        return PF_NOR_INVALID_ARGUMENT;
    }
    pf_nor_result_t result = pf_nor_flash_sync(flash);
    if (result != PF_NOR_OK) {
        return result;
    }
    return execute(flash, flash->quad_enabled ? 0x6bu : 0x03u, address, 3u,
                   NULL, buffer, length, flash->quad_enabled ? 4u : 1u,
                   flash->quad_enabled ? 8u : 0u);
}

pf_nor_result_t pf_nor_flash_program(pf_nor_flash_t *flash, uint32_t address,
                                    const void *buffer, size_t length)
{
    if (flash == NULL || buffer == NULL || length == 0u) {
        return PF_NOR_INVALID_ARGUMENT;
    }
    if (!flash->initialized) {
        return PF_NOR_NOT_READY;
    }
    if (!range_valid(flash, address, length)) {
        return PF_NOR_INVALID_ARGUMENT;
    }
    const uint8_t *bytes = buffer;
    while (length != 0u) {
        size_t chunk = flash->page_size - address % flash->page_size;
        if (chunk > length) {
            chunk = length;
        }
        pf_nor_result_t result = pf_nor_flash_sync(flash);
        if (result == PF_NOR_OK) {
            result = write_enable(flash);
        }
        if (result == PF_NOR_OK) {
            result = execute(flash, 0x02u, address, 3u, bytes, NULL, chunk, 1u, 0u);
        }
        if (result == PF_NOR_OK) {
            result = wait_ready(flash, 100000u);
        }
        if (result == PF_NOR_OK) {
            result = verify(flash, address, bytes, chunk);
        }
        if (result != PF_NOR_OK) {
            return result;
        }
        bytes += chunk;
        address += (uint32_t)chunk;
        length -= chunk;
    }
    return PF_NOR_OK;
}

pf_nor_result_t pf_nor_flash_erase(pf_nor_flash_t *flash, uint32_t address)
{
    if (flash == NULL) {
        return PF_NOR_INVALID_ARGUMENT;
    }
    if (!flash->initialized) {
        return PF_NOR_NOT_READY;
    }
    if (!range_valid(flash, address, flash->erase_size) || address % flash->erase_size != 0u) {
        return PF_NOR_INVALID_ARGUMENT;
    }
    pf_nor_result_t result = pf_nor_flash_sync(flash);
    if (result == PF_NOR_OK) {
        result = write_enable(flash);
    }
    if (result == PF_NOR_OK) {
        result = execute(flash, 0x20u, address, 3u, NULL, NULL, 0u, 1u, 0u);
    }
    if (result == PF_NOR_OK) {
        result = wait_ready(flash, 3000000u);
    }
    return result == PF_NOR_OK ? verify(flash, address, NULL, flash->erase_size) : result;
}

pf_nor_result_t pf_nor_flash_enable_quad(pf_nor_flash_t *flash)
{
    if (flash == NULL) {
        return PF_NOR_INVALID_ARGUMENT;
    }
    if (!flash->initialized) {
        return PF_NOR_NOT_READY;
    }
    if (!flash->transport.quad_supported) {
        return PF_NOR_UNSUPPORTED;
    }
    pf_nor_result_t result = pf_nor_flash_sync(flash);
    uint8_t value;
    if (result == PF_NOR_OK) {
        result = status(flash, 0x35u, &value);
    }
    if (result != PF_NOR_OK) {
        return result;
    }
    if ((value & 2u) == 0u) {
        value |= 2u;
        result = execute(flash, 0x50u, 0u, 0u, NULL, NULL, 0u, 1u, 0u);
        if (result == PF_NOR_OK) {
            result = execute(flash, 0x31u, 0u, 0u, &value, NULL, 1u, 1u, 0u);
        }
        if (result == PF_NOR_OK) {
            result = wait_ready(flash, 100000u);
        }
        if (result == PF_NOR_OK) {
            result = status(flash, 0x35u, &value);
        }
    }
    if (result == PF_NOR_OK && (value & 2u) == 0u) {
        result = PF_NOR_VERIFY_ERROR;
    }
    flash->quad_enabled = result == PF_NOR_OK;
    return result;
}
