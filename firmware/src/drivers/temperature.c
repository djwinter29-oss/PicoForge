#include "temperature.h"

#include "hardware/adc.h"

enum { PICOFORGE_TEMPERATURE_ADC_CHANNEL = 4u };

static bool temperature_initialized;

void picoforge_temperature_init(void)
{
    adc_init();
    adc_set_temp_sensor_enabled(true);
    adc_select_input(PICOFORGE_TEMPERATURE_ADC_CHANNEL);
    temperature_initialized = true;
}

bool picoforge_temperature_read_centidegrees(int16_t *temperature)
{
    if (!temperature_initialized || (temperature == NULL)) {
        return false;
    }

    adc_select_input(PICOFORGE_TEMPERATURE_ADC_CHANNEL);
    *temperature = picoforge_temperature_convert_raw(adc_read());
    return true;
}