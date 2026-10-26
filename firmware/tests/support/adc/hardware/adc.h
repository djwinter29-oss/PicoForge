#ifndef HARDWARE_ADC_H
#define HARDWARE_ADC_H

#include <stdbool.h>
#include <stdint.h>

#define ADC_TEMPERATURE_CHANNEL_NUM 4u

void adc_init(void);
void adc_set_temp_sensor_enabled(bool enable);
void adc_select_input(unsigned input);
uint16_t adc_read(void);

#endif
