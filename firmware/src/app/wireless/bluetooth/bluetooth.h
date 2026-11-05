/**
 * @file bluetooth.h
 * @brief Internal BLE peripheral lifecycle, called only by the USB wireless owner task.
 */
#ifndef PF_BLUETOOTH_H
#define PF_BLUETOOTH_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Load the boot control key once before SDK initialization.
 * @return true only when the Bluetooth switch is on and a control key is saved.
 */
bool pf_bluetooth_prepare(void);

/** @brief Initialize GATT, authorization callbacks and advertising after shared SDK initialization.
 * @return true when the controller power-on request succeeds; failures are logged.
 */
bool pf_bluetooth_start(void);

/** @brief Report the peripheral link for Feature 12 byte 3.
 * @return 0 when the controller is not working, 1 while advertising, 2 while a central is connected.
 */
uint8_t pf_bluetooth_state(void);

/** @brief Apply owner-task BLE timers, including the fast-to-slow advertising transition. */
void pf_bluetooth_poll(void);

#endif
