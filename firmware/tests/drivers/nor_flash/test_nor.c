#include "unity.h"
#include "bus.h"
#include "nor.h"

#include <string.h>

static pf_nor_t flash;
static uint8_t memory[8192];
static uint8_t sr1, sr2;
static uint8_t id[3];
static uint64_t ticks;
static bool stuck, allow_write, allow_quad, protected;
static bool volatile_status;
static unsigned programs, erases, calls;
static pf_nor_result_t injected;
static pf_nor_result_t uid_result;
static const uint8_t unique_id[8] = {0x01u, 0x23u, 0x45u, 0x67u, 0x89u, 0xabu, 0xcdu, 0xefu};

static uint64_t now(void *context)
{
    (void)context;
    ticks += 10000u;
    return ticks;
}

static pf_nor_result_t execute(void *context, const pf_nor_transaction_t *transaction)
{
    (void)context;
    calls++;
    TEST_ASSERT_TRUE(pf_nor_transaction_valid(transaction));
    if (injected != PF_NOR_OK) {
        return injected;
    }
    switch (transaction->command) {
    case 0x05:
        transaction->rx[0] = sr1 | (stuck ? 1u : 0u);
        break;
    case 0x35:
        transaction->rx[0] = sr2;
        break;
    case 0x9f:
        memcpy(transaction->rx, id, 3u);
        break;
    case 0x4b:
        TEST_ASSERT_EQUAL_UINT8(0u, transaction->address_bytes);
        TEST_ASSERT_EQUAL_UINT8(32u, transaction->dummy_cycles);
        TEST_ASSERT_EQUAL_UINT8(1u, transaction->data_lanes);
        TEST_ASSERT_EQUAL_UINT32(8u, transaction->length);
        if (uid_result != PF_NOR_OK) return uid_result;
        memcpy(transaction->rx, unique_id, sizeof(unique_id));
        break;
    case 0x06:
        if (allow_write) {
            sr1 |= 2u;
        }
        break;
    case 0x50:
        volatile_status = true;
        break;
    case 0x31:
        TEST_ASSERT_TRUE(volatile_status);
        if (allow_quad) {
            sr2 = transaction->tx[0];
        }
        volatile_status = false;
        break;
    case 0x03:
    case 0x6b:
        TEST_ASSERT_EQUAL_UINT32(3u, transaction->address_bytes);
        TEST_ASSERT_TRUE(transaction->address + transaction->length <= sizeof(memory));
        if (transaction->command == 0x6b) {
            TEST_ASSERT_EQUAL_UINT8(4u, transaction->data_lanes);
            TEST_ASSERT_EQUAL_UINT8(8u, transaction->dummy_cycles);
            TEST_ASSERT_BITS_HIGH(2u, sr2);
        }
        memcpy(transaction->rx, memory + transaction->address, transaction->length);
        break;
    case 0x02:
        TEST_ASSERT_BITS_HIGH(2u, sr1);
        TEST_ASSERT_TRUE(transaction->length <= 256u - transaction->address % 256u);
        TEST_ASSERT_TRUE(transaction->address + transaction->length <= sizeof(memory));
        for (size_t index = 0u; index < transaction->length; index++) {
            if (!protected) {
                memory[transaction->address + index] &= transaction->tx[index];
            }
        }
        sr1 &= (uint8_t)~2u;
        programs++;
        break;
    case 0x20:
        TEST_ASSERT_BITS_HIGH(2u, sr1);
        TEST_ASSERT_EQUAL_UINT32(0u, transaction->address % 4096u);
        TEST_ASSERT_TRUE(transaction->address + 4096u <= sizeof(memory));
        if (!protected) {
            memset(memory + transaction->address, 0xff, 4096u);
        }
        sr1 &= (uint8_t)~2u;
        erases++;
        break;
    default:
        TEST_FAIL_MESSAGE("Unexpected NOR command");
    }
    return PF_NOR_OK;
}

void setUp(void)
{
    memset(&flash, 0, sizeof(flash));
    memset(memory, 0xff, sizeof(memory));
    sr1 = 0u;
    sr2 = 0xa0u;
    id[0] = 0xefu;
    id[1] = 0x40u;
    id[2] = 0x18u;
    ticks = 0u;
    stuck = false;
    protected = false;
    allow_write = allow_quad = true;
    volatile_status = false;
    programs = erases = calls = 0u;
    injected = PF_NOR_OK;
    uid_result = PF_NOR_OK;
}

void tearDown(void) {}

static void init(void)
{
    pf_nor_transport_t transport = {
        .execute = execute, .time_us = now, .quad_supported = true,
    };
    TEST_ASSERT_EQUAL(PF_NOR_OK, pf_nor_init(&flash, &transport));
}

void test_identifies_geometry_without_modifying_array(void)
{
    init();
    TEST_ASSERT_EQUAL_UINT32(16777216u, flash.capacity_bytes);
    TEST_ASSERT_EQUAL_UINT32(256u, flash.page_size);
    TEST_ASSERT_EQUAL_UINT32(4096u, flash.erase_size);
    TEST_ASSERT_EQUAL_MEMORY(unique_id, flash.unique_id, sizeof(unique_id));
    TEST_ASSERT_EQUAL_UINT32(0u, programs + erases);
}

void test_uid_read_failure_prevents_successful_initialization(void)
{
    uid_result = PF_NOR_IO_ERROR;
    pf_nor_transport_t transport = {.execute = execute, .time_us = now};
    TEST_ASSERT_EQUAL(PF_NOR_IO_ERROR, pf_nor_init(&flash, &transport));
    TEST_ASSERT_FALSE(flash.initialized);
    TEST_ASSERT_EQUAL_UINT32(0u, programs + erases);
}

void test_rejects_unknown_part_and_uninitialized_access(void)
{
    uint8_t buffer;
    TEST_ASSERT_EQUAL(PF_NOR_NOT_READY, pf_nor_read(&flash, 0u, &buffer, 1u));
    id[0] = 0u;
    pf_nor_transport_t transport = {.execute = execute, .time_us = now};
    TEST_ASSERT_EQUAL(PF_NOR_UNSUPPORTED, pf_nor_init(&flash, &transport));
    TEST_ASSERT_FALSE(flash.initialized);
}

void test_splits_program_at_page_boundaries_and_reads_back(void)
{
    init();
    uint8_t input[300], output[300];
    memset(input, 0x5a, sizeof(input));
    TEST_ASSERT_EQUAL(PF_NOR_OK, pf_nor_program(&flash, 250u, input, sizeof(input)));
    TEST_ASSERT_EQUAL_UINT32(3u, programs);
    TEST_ASSERT_EQUAL(PF_NOR_OK, pf_nor_read(&flash, 250u, output, sizeof(output)));
    TEST_ASSERT_EQUAL_MEMORY(input, output, sizeof(input));
    TEST_ASSERT_EQUAL_UINT8(0xffu, memory[249]);
    TEST_ASSERT_EQUAL_UINT8(0xffu, memory[550]);
}

void test_erase_is_explicit_aligned_and_confined_to_sector(void)
{
    init();
    memset(memory, 0, sizeof(memory));
    TEST_ASSERT_EQUAL(PF_NOR_INVALID_ARGUMENT, pf_nor_erase(&flash, 1u));
    TEST_ASSERT_EQUAL_UINT32(0u, erases);
    TEST_ASSERT_EQUAL(PF_NOR_OK, pf_nor_erase(&flash, 0u));
    TEST_ASSERT_EQUAL_UINT8(0xffu, memory[4095]);
    TEST_ASSERT_EQUAL_UINT8(0u, memory[4096]);
}

void test_rejects_bad_ranges_and_buffers_without_bus_io(void)
{
    init();
    unsigned before = calls;
    uint8_t data[2];
    TEST_ASSERT_EQUAL(PF_NOR_INVALID_ARGUMENT, pf_nor_read(&flash, 16777215u, data, 2u));
    TEST_ASSERT_EQUAL(PF_NOR_INVALID_ARGUMENT, pf_nor_program(&flash, UINT32_MAX, data, 2u));
    TEST_ASSERT_EQUAL(PF_NOR_INVALID_ARGUMENT, pf_nor_read(&flash, 0u, NULL, 2u));
    TEST_ASSERT_EQUAL(PF_NOR_INVALID_ARGUMENT, pf_nor_read(&flash, 0u, data, 0u));
    TEST_ASSERT_EQUAL_UINT32(before, calls);
}

void test_write_enable_failure_prevents_program_and_erase(void)
{
    init();
    allow_write = false;
    uint8_t data = 0u;
    TEST_ASSERT_EQUAL(PF_NOR_VERIFY_ERROR, pf_nor_program(&flash, 0u, &data, 1u));
    TEST_ASSERT_EQUAL(PF_NOR_VERIFY_ERROR, pf_nor_erase(&flash, 0u));
    TEST_ASSERT_EQUAL_UINT32(0u, programs + erases);
}

void test_busy_timeout_and_transport_error_propagate(void)
{
    init();
    stuck = true;
    TEST_ASSERT_EQUAL(PF_NOR_TIMEOUT, pf_nor_sync(&flash));
    stuck = false;
    injected = PF_NOR_IO_ERROR;
    TEST_ASSERT_EQUAL(PF_NOR_IO_ERROR, pf_nor_sync(&flash));
}

void test_quad_enable_preserves_status_and_quad_read_matches_single(void)
{
    init();
    uint8_t single[16], quad[16];
    TEST_ASSERT_EQUAL(PF_NOR_OK, pf_nor_read(&flash, 0u, single, sizeof(single)));
    TEST_ASSERT_EQUAL(PF_NOR_OK, pf_nor_enable_quad(&flash));
    TEST_ASSERT_EQUAL_HEX8(0xa2u, sr2);
    TEST_ASSERT_EQUAL(PF_NOR_OK, pf_nor_read(&flash, 0u, quad, sizeof(quad)));
    TEST_ASSERT_EQUAL_MEMORY(single, quad, sizeof(single));
    TEST_ASSERT_EQUAL_UINT32(0u, programs + erases);
}

void test_quad_enable_failure_and_unsupported_transport_are_explicit(void)
{
    init();
    allow_quad = false;
    TEST_ASSERT_EQUAL(PF_NOR_VERIFY_ERROR, pf_nor_enable_quad(&flash));
    TEST_ASSERT_FALSE(flash.quad_enabled);
    flash.transport.quad_supported = false;
    TEST_ASSERT_EQUAL(PF_NOR_UNSUPPORTED, pf_nor_enable_quad(&flash));
}

void test_transaction_shape_rejects_ambiguous_and_out_of_range_requests(void)
{
    uint8_t data;
    pf_nor_transaction_t transaction = {.data_lanes = 1u};
    TEST_ASSERT_TRUE(pf_nor_transaction_valid(&transaction));
    transaction.length = 1u;
    TEST_ASSERT_FALSE(pf_nor_transaction_valid(&transaction));
    transaction.rx = &data;
    TEST_ASSERT_TRUE(pf_nor_transaction_valid(&transaction));
    transaction.tx = &data;
    TEST_ASSERT_FALSE(pf_nor_transaction_valid(&transaction));
    transaction.tx = NULL;
    transaction.address_bytes = 4u;
    TEST_ASSERT_FALSE(pf_nor_transaction_valid(&transaction));
}

void test_program_cannot_silently_restore_zero_bits_to_one(void)
{
    init();
    memory[0] = 0u;
    uint8_t data = 0xffu;
    TEST_ASSERT_EQUAL(PF_NOR_VERIFY_ERROR, pf_nor_program(&flash, 0u, &data, 1u));
    TEST_ASSERT_EQUAL_UINT32(0u, erases);
}

void test_ignored_protected_program_and_erase_fail_verification(void)
{
    init();
    protected = true;
    uint8_t data = 0u;
    TEST_ASSERT_EQUAL(PF_NOR_VERIFY_ERROR, pf_nor_program(&flash, 0u, &data, 1u));
    memory[0] = 0u;
    TEST_ASSERT_EQUAL(PF_NOR_VERIFY_ERROR, pf_nor_erase(&flash, 0u));
}
