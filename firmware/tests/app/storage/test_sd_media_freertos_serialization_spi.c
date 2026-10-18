#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/storage/serialization")
TEST_INCLUDE_PATH("firmware/tests/support/storage")
#include "storage/sd_media_serialization.inc"
void setUp(void) { alarm(15); }
void tearDown(void) { alarm(0); }
void test_spi_media_contracts_and_concurrent_reservations(void)
{
    check_contracts();
    check_serialization();
}
