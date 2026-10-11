#ifndef PF_TEMPERATURE_H
#define PF_TEMPERATURE_H

#include <stdbool.h>
#include <limits.h>
#include <stdint.h>

/**
 * Convert a 12-bit RP2 temperature ADC sample to saturated centidegrees C.
 * The conversion uses the nominal 3.3 V ADC reference and RP2 sensor formula.
 */
static inline int16_t pf_temperature_convert_raw(uint16_t raw_sample)
{
	uint32_t sample = (raw_sample > 4095u) ? 4095u : raw_sample;
	int64_t voltage_delta = ((int64_t)sample * 3300) - ((int64_t)706 * 4096);
	int64_t centidegrees = 2700 -
		((voltage_delta * 100000) / ((int64_t)4096 * 1721));

	if (centidegrees > INT16_MAX) {
		return INT16_MAX;
	}
	if (centidegrees < INT16_MIN) {
		return INT16_MIN;
	}
	return (int16_t)centidegrees;
}

/** Initialize the MCU's internal ADC temperature sensor. */
void pf_temperature_init(void);

/** Read temperature in centidegrees Celsius; returns false before initialization. */
bool pf_temperature_read_centidegrees(int16_t *temperature);

#endif