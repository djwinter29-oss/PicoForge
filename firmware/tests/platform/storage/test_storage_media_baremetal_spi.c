#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/storage")
#include "storage/storage_media_baremetal.inc"
void setUp(void) {}
void tearDown(void) {}
void test_spi_media_baremetal_bytes_ownership_and_sync(void) { check_baremetal(); }
