#include "unity.h"

TEST_INCLUDE_PATH("firmware/tests/support/adc")

#include "hardware/adc.h"
#include "temperature.h"

#include <limits.h>

static unsigned init_calls, selected_input, sensor_enabled;
static uint16_t sample;

void adc_init(void) { ++init_calls; }
void adc_set_temp_sensor_enabled(bool enable) { sensor_enabled = enable; }
void adc_select_input(unsigned input) { selected_input = input; }
uint16_t adc_read(void) { return sample; }

void setUp(void)
{
    selected_input = 0u;
    sample = 876u;
}
void tearDown(void) {}

void test_read_fails_before_init_and_for_null(void)
{
    int16_t value = 0;
    TEST_ASSERT_FALSE(pf_temperature_read_centidegrees(&value));
    pf_temperature_init();
    TEST_ASSERT_FALSE(pf_temperature_read_centidegrees(NULL));
}

void test_init_is_idempotent_and_read_selects_the_temperature_channel(void)
{
    int16_t value = 0;
    pf_temperature_init();
    TEST_ASSERT_EQUAL_UINT(1u, init_calls);
    TEST_ASSERT_TRUE(sensor_enabled);
    TEST_ASSERT_TRUE(pf_temperature_read_centidegrees(&value));
    TEST_ASSERT_EQUAL_UINT(ADC_TEMPERATURE_CHANNEL_NUM, selected_input);
    TEST_ASSERT_EQUAL_INT16(2713, value);
}

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