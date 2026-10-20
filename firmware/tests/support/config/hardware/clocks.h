#ifndef HARDWARE_CLOCKS_H
#define HARDWARE_CLOCKS_H

#include <stdbool.h>
#include <stdint.h>

typedef unsigned uint;
enum clock_num_rp { clk_peri = 1, clk_sys = 2, clk_hstx = 3 };

#define HAS_HSTX 1
#define SYS_CLK_VREG_VOLTAGE_AUTO_ADJUST_DELAY_US 1000
#define CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS 0u
#define CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS 0u

uint32_t clock_get_hz(enum clock_num_rp clock);
bool check_sys_clock_khz(uint32_t freq_khz, uint *vco_freq_out, uint *post_div1_out, uint *post_div2_out);
void set_sys_clock_pll(uint32_t vco_freq, uint post_div1, uint post_div2);
bool clock_configure_undivided(enum clock_num_rp clock, uint32_t src, uint32_t auxsrc, uint32_t src_freq);
void busy_wait_at_least_cycles(uint32_t minimum_cycles);

#endif
