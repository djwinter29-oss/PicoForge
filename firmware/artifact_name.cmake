# Use the SDK platform identity, not a particular board name.
if(PICO_RP2040)
    set(pf_image_mcu rp2040)
    set(pf_image_clock 125)
elseif(PICO_RP2350)
    set(pf_image_mcu rp2350)
    set(pf_image_clock 150)
else()
    message(FATAL_ERROR "PicoForge artifact naming requires an RP2040 or RP2350 platform")
endif()
# SDK stock clocks above are checked against SYS_CLK_HZ in main.c.
string(TIMESTAMP pf_image_date "%Y%m%d" UTC)
set(PF_ARTIFACT_NAME "pico_forge_${pf_image_mcu}_${pf_image_clock}_${pf_image_date}")
message(STATUS "Firmware image: ${PF_ARTIFACT_NAME}")
