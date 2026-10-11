#include "unity.h"
#include "temperature.h"

#include <limits.h>

void setUp(void) {}
void tearDown(void) {}

void test_converts_the_nominal_27_degree_reference_sample(void)
{
    TEST_ASSERT_EQUAL_INT16(2713, pf_temperature_convert_raw(876u));
}

void test_saturates_extreme_adc_samples_to_hid_range(void)
{
    TEST_ASSERT_EQUAL_INT16(INT16_MAX, pf_temperature_convert_raw(0u));
    TEST_ASSERT_EQUAL_INT16(INT16_MIN, pf_temperature_convert_raw(4095u));
    TEST_ASSERT_EQUAL_INT16(INT16_MIN, pf_temperature_convert_raw(UINT16_MAX));
}