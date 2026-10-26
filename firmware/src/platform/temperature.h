/**
 * @file temperature.h
 * @brief On-chip ADC temperature sensor driver.
 */
#ifndef PF_TEMPERATURE_H
#define PF_TEMPERATURE_H

#include <stdbool.h>
#include <limits.h>
#include <stdint.h>

#define PF_TEMPERATURE_ADC_REFERENCE_MV 3300 /**< Nominal ADC reference voltage in millivolts. */
#define PF_TEMPERATURE_SENSOR_27C_MV 706 /**< Sensor output at 27 degrees C in millivolts. */
#define PF_TEMPERATURE_SLOPE_UV_PER_C 1721 /**< Sensor slope in microvolts per degree C (negative-going). */

/**
 * @brief Convert a 12-bit RP2 temperature ADC sample to saturated centidegrees C.
 * @param raw_sample ADC sample; values above 4095 are clamped.
 * @return Centidegrees Celsius, saturated to the int16_t range.
 * @note Uses the nominal 3.3 V ADC reference and the RP2 sensor formula. The sensor is only accurate to a few
 *       degrees without per-chip calibration, so treat the result as an estimate.
 */
static inline int16_t pf_temperature_convert_raw(uint16_t raw_sample)
{
    uint32_t sample = (raw_sample > 4095u) ? 4095u : raw_sample;
    int64_t voltage_delta = ((int64_t)sample * PF_TEMPERATURE_ADC_REFERENCE_MV) -
                            ((int64_t)PF_TEMPERATURE_SENSOR_27C_MV * 4096);
    int64_t centidegrees = 2700 - ((voltage_delta * 100000) / ((int64_t)4096 * PF_TEMPERATURE_SLOPE_UV_PER_C));

    if (centidegrees > INT16_MAX) {
        return INT16_MAX;
    }
    if (centidegrees < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)centidegrees;
}

/**
 * @brief Initialize the ADC and enable the temperature sensor; later calls are no-ops.
 * @note This driver owns the ADC: it calls adc_init() and leaves the temperature channel selected.
 */
void pf_temperature_init(void);

/**
 * @brief Read the die temperature.
 * @param temperature Receives centidegrees Celsius; must not be NULL.
 * @return true on success; false before initialization or for a NULL pointer.
 * @note Not safe for concurrent callers: the input is selected and then read without locking.
 */
bool pf_temperature_read_centidegrees(int16_t *temperature);

#endif
