/**
 * @file temperature.c
 * @brief On-chip ADC temperature sensor driver.
 */
#include "temperature.h"

#include <stddef.h>

#include "hardware/adc.h"

static bool temperature_initialized;

void pf_temperature_init(void)
{
    if (temperature_initialized) {
        return;
    }
    adc_init();
    adc_set_temp_sensor_enabled(true);
    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
    temperature_initialized = true;
}

/* ponytail: select-then-read is unlocked, so concurrent callers or another ADC user can race. Ceiling: one reader
 * (the USB task) today. Upgrade path: take a mutex around select+read if a second ADC user appears. */
bool pf_temperature_read_centidegrees(int16_t *temperature)
{
    if (!temperature_initialized || (temperature == NULL)) {
        return false;
    }

    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
    *temperature = pf_temperature_convert_raw(adc_read());
    return true;
}
