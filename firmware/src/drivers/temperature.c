#include "temperature.h"

#include "hardware/adc.h"

enum { PF_TEMPERATURE_ADC_CHANNEL = 4u };

static bool temperature_initialized;

void pf_temperature_init(void)
{
    adc_init();
    adc_set_temp_sensor_enabled(true);
    adc_select_input(PF_TEMPERATURE_ADC_CHANNEL);
    temperature_initialized = true;
}

bool pf_temperature_read_centidegrees(int16_t *temperature)
{
    if (!temperature_initialized || (temperature == NULL)) {
        return false;
    }

    adc_select_input(PF_TEMPERATURE_ADC_CHANNEL);
    *temperature = pf_temperature_convert_raw(adc_read());
    return true;
}