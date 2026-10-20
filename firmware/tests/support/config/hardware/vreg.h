#ifndef HARDWARE_VREG_H
#define HARDWARE_VREG_H

enum vreg_voltage {
    VREG_VOLTAGE_1_10 = 0b01011,
    VREG_VOLTAGE_1_15 = 0b01100,
    VREG_VOLTAGE_1_30 = 0b01111,
};

void vreg_set_voltage(enum vreg_voltage voltage);
enum vreg_voltage vreg_get_voltage(void);

#endif
