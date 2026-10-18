#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/storage")
#include "../../support/storage/facade_baremetal.inc"
void setUp(void) {}
void tearDown(void) {}
void test_spi_baremetal_port_bytes_ownership_and_sync(void) { check_baremetal(); }
