#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/storage/serialization")
TEST_INCLUDE_PATH("firmware/tests/support/storage")
#include "../../support/storage/facade_serialization.inc"
void setUp(void) { alarm(15); }
void tearDown(void) { alarm(0); }
void test_sdio_facade_contracts_and_concurrent_reservations(void)
{
    check_contracts();
    check_serialization();
}
