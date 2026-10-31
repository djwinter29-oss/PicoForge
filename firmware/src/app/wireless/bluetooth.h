/**
 * @file bluetooth.h
 * @brief Internal BLE peripheral lifecycle, called only by the USB wireless owner task.
 */
#ifndef PF_BLUETOOTH_H
#define PF_BLUETOOTH_H

#include <stdbool.h>

/** @brief Load the boot control key once before SDK initialization.
 * @return true when the Bluetooth master switch and nonzero key allow startup.
 */
bool pf_bluetooth_prepare(void);

/** @brief Initialize GATT, authorization callbacks and advertising after shared SDK initialization.
 * @return true when the controller power-on request succeeds; failures are logged.
 */
bool pf_bluetooth_start(void);

#endif
