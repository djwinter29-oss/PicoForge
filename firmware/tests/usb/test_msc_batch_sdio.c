#include "unity.h"

TEST_INCLUDE_PATH("firmware/tests/support/msc")
TEST_SOURCE_FILE("sd.c")
TEST_SOURCE_FILE("sd_port.c")

#include "msc_batch_cases.inc"

void setUp(void) {}
void tearDown(void) {}

void test_sdio_batches_preserve_bytes_bounds_ownership_and_completion(void)
{
    msc_batch_regression();
}
