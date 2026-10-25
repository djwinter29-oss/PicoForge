#include "unity.h"
#include "sd_link.h"

static int context;

void setUp(void) {}
void tearDown(void) {}

static bool absent(const void *value)
{
    TEST_ASSERT_EQUAL_PTR(&context, value);
    return false;
}

void test_descriptor_validation_rejects_missing_context_or_wrong_mode(void)
{
    pf_sd_link_t bus = {.context = &context, .mode = PF_SD_LINK_SPI};
    TEST_ASSERT_TRUE(pf_sd_link_valid(&bus, PF_SD_LINK_SPI));
    TEST_ASSERT_FALSE(pf_sd_link_valid(&bus, PF_SD_LINK_NATIVE_SD));
    TEST_ASSERT_FALSE(pf_sd_link_valid(NULL, PF_SD_LINK_SPI));
    bus.context = NULL;
    TEST_ASSERT_FALSE(pf_sd_link_valid(&bus, PF_SD_LINK_SPI));
    bus.context = &context;
    bus.mode = (pf_sd_link_mode_t)2;
    TEST_ASSERT_FALSE(pf_sd_link_valid(&bus, bus.mode));
}

void test_presence_and_descriptor_queries_keep_backend_state_opaque(void)
{
    pf_sd_link_t bus = {.context = &context, .mode = PF_SD_LINK_SPI};
    pf_sd_link_t copy = bus;
    TEST_ASSERT_TRUE(pf_sd_link_same(&bus, &copy));
    TEST_ASSERT_FALSE(pf_sd_link_presence_known(&bus));
    TEST_ASSERT_TRUE(pf_sd_link_present(&bus));
    bus.present = absent;
    bus.presence_context = &context;
    TEST_ASSERT_FALSE(pf_sd_link_same(&bus, &copy));
    TEST_ASSERT_TRUE(pf_sd_link_presence_known(&bus));
    TEST_ASSERT_FALSE(pf_sd_link_present(&bus));
    TEST_ASSERT_FALSE(pf_sd_link_present(NULL));
    TEST_ASSERT_FALSE(pf_sd_link_same(NULL, &copy));
}
