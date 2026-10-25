cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(DEFINED PICO_SDK_PATH)
    set(PICO_SDK_PATH_VALUE "${PICO_SDK_PATH}")
elseif(DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH_VALUE "$ENV{PICO_SDK_PATH}")
else()
    set(PICO_SDK_PATH_VALUE "${CMAKE_CURRENT_LIST_DIR}/third_party/pico-sdk")
endif()

include("${PICO_SDK_PATH_VALUE}/external/pico_sdk_import.cmake")

if(DEFINED FREERTOS_KERNEL_PATH)
    set(FREERTOS_KERNEL_PATH_VALUE "${FREERTOS_KERNEL_PATH}")
elseif(DEFINED ENV{FREERTOS_KERNEL_PATH})
    set(FREERTOS_KERNEL_PATH_VALUE "$ENV{FREERTOS_KERNEL_PATH}")
else()
    set(FREERTOS_KERNEL_PATH_VALUE "${CMAKE_CURRENT_LIST_DIR}/third_party/FreeRTOS-Kernel")
endif()

project(pico_forge C CXX ASM)

set(PF_FIRMWARE_VERSION "0.0.0-dev" CACHE STRING "Firmware semantic version string")
include("${CMAKE_CURRENT_LIST_DIR}/version.cmake")

set(PF_SDCARD_TRANSPORT "SDIO_PIO" CACHE STRING "SD-card transport: SDIO_PIO or SPI")
set_property(CACHE PF_SDCARD_TRANSPORT PROPERTY STRINGS SDIO_PIO SPI)
option(PF_SDCARD_INTERRUPT_WAIT "Enable experimental FreeRTOS native PIO completion waits" OFF)
set(PF_SDCARD_PORT "FREERTOS" CACHE STRING "SD-card execution environment: FREERTOS or BAREMETAL")
set_property(CACHE PF_SDCARD_PORT PROPERTY STRINGS FREERTOS BAREMETAL)
if(NOT PF_SDCARD_PORT STREQUAL "FREERTOS" AND NOT PF_SDCARD_PORT STREQUAL "BAREMETAL")
    message(FATAL_ERROR "PF_SDCARD_PORT must be FREERTOS or BAREMETAL")
endif()
if(PF_SDCARD_INTERRUPT_WAIT AND NOT PF_SDCARD_PORT STREQUAL "FREERTOS")
    message(FATAL_ERROR "PF_SDCARD_INTERRUPT_WAIT requires PF_SDCARD_PORT=FREERTOS")
endif()
if(PF_SDCARD_TRANSPORT STREQUAL "SDIO_PIO")
    set(PF_CONFIG_SDCARD_TRANSPORT_VALUE 0)
elseif(PF_SDCARD_TRANSPORT STREQUAL "SPI")
    set(PF_CONFIG_SDCARD_TRANSPORT_VALUE 1)
    if(PF_SDCARD_INTERRUPT_WAIT)
        message(FATAL_ERROR "PF_SDCARD_INTERRUPT_WAIT requires PF_SDCARD_TRANSPORT=SDIO_PIO")
    endif()
else()
    message(FATAL_ERROR "PF_SDCARD_TRANSPORT must be SDIO_PIO or SPI")
endif()
add_compile_definitions(PF_CONFIG_SDCARD_TRANSPORT=${PF_CONFIG_SDCARD_TRANSPORT_VALUE})

set(PF_SYS_CLK_MHZ 0 CACHE STRING
    "Out-of-spec system clock in MHz (100-250, 12 MHz crystal); 0 keeps the SDK default")
if(NOT PF_SYS_CLK_MHZ MATCHES "^[0-9]+$" OR
   (NOT PF_SYS_CLK_MHZ EQUAL 0 AND (PF_SYS_CLK_MHZ LESS 100 OR PF_SYS_CLK_MHZ GREATER 250)))
    message(FATAL_ERROR "PF_SYS_CLK_MHZ must be 0 or 100-250, got '${PF_SYS_CLK_MHZ}'")
endif()
if(NOT PF_SYS_CLK_MHZ EQUAL 0)
    # Mirror the SDK's PLL search so the clock is applied during SDK runtime init, before USB and stdio start.
    set(pf_best_vco 0)
    foreach(pf_step RANGE 0 6)
        math(EXPR pf_postdiv1 "7 - ${pf_step}")
        foreach(pf_postdiv2 RANGE 1 ${pf_postdiv1})
            math(EXPR pf_vco "${PF_SYS_CLK_MHZ} * ${pf_postdiv1} * ${pf_postdiv2}")
            math(EXPR pf_vco_remainder "${pf_vco} % 12")
            if(pf_vco_remainder EQUAL 0 AND pf_vco GREATER 749 AND pf_vco LESS 1601 AND
               pf_vco GREATER pf_best_vco)
                set(pf_best_vco ${pf_vco})
                set(pf_best_postdiv1 ${pf_postdiv1})
                set(pf_best_postdiv2 ${pf_postdiv2})
            endif()
        endforeach()
    endforeach()
    if(pf_best_vco EQUAL 0)
        message(FATAL_ERROR "PF_SYS_CLK_MHZ=${PF_SYS_CLK_MHZ} cannot be derived from a 12 MHz crystal")
    endif()
    message(STATUS "Overclocking system clock to ${PF_SYS_CLK_MHZ} MHz (out of spec)")
    if(PF_SYS_CLK_MHZ GREATER 200)
        set(pf_vreg VREG_VOLTAGE_1_30)
    else()
        set(pf_vreg VREG_VOLTAGE_1_15)
    endif()
    # Raise the regulator during clock init so the core has voltage margin above the stock speed.
    add_compile_definitions(
        SYS_CLK_HZ=${PF_SYS_CLK_MHZ}000000
        PLL_SYS_VCO_FREQ_HZ=${pf_best_vco}000000
        PLL_SYS_POSTDIV1=${pf_best_postdiv1}
        PLL_SYS_POSTDIV2=${pf_best_postdiv2}
        SYS_CLK_VREG_VOLTAGE_AUTO_ADJUST=1
        SYS_CLK_VREG_VOLTAGE_MIN=${pf_vreg}
    )
endif()

pico_sdk_init()

set(PF_HID_BOARD_MODEL_VALUE 0)
if(PICO_BOARD STREQUAL "pico")
    set(PF_HID_BOARD_MODEL_VALUE 1)
elseif(PICO_BOARD STREQUAL "pico2")
    set(PF_HID_BOARD_MODEL_VALUE 2)
elseif(PICO_BOARD STREQUAL "pico2_w")
    set(PF_HID_BOARD_MODEL_VALUE 3)
endif()
add_compile_definitions(PF_HID_BOARD_MODEL=${PF_HID_BOARD_MODEL_VALUE})

add_library(pf_sd_link STATIC
    src/drivers/sdcard/link/sd_link.c
    src/drivers/sdcard/link/sd_link_serial.c
    src/drivers/sdcard/link/sd_link_wire.c)
target_include_directories(pf_sd_link PUBLIC "${CMAKE_CURRENT_LIST_DIR}/src")

if(PF_SDCARD_PORT STREQUAL "FREERTOS")
    set(FREERTOS_KERNEL_PATH "${FREERTOS_KERNEL_PATH_VALUE}" CACHE PATH "Path to the pinned FreeRTOS-Kernel checkout")
    set(FREERTOS_CONFIG_FILE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/src/integration/freertos")
    include("${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2040/FreeRTOS_Kernel_import.cmake")
endif()

add_library(pf_sdcard_protocol STATIC
    src/drivers/sdcard/sd_protocol.c
)
target_include_directories(pf_sdcard_protocol PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers/sdcard"
)
# Let SPI-only firmware discard unused native codecs and their CRC tables.
target_compile_options(pf_sdcard_protocol PRIVATE -ffunction-sections -fdata-sections)

if(PF_SDCARD_TRANSPORT STREQUAL "SDIO_PIO")
    add_library(pf_sdio_backend STATIC
        src/drivers/sdcard/sd_native.c
        src/drivers/sdcard/sd_native_host.c
        src/drivers/sdcard/sdio_pio/sdio_pio.c
    )
    option(PF_SDIO_PIO_FUSED_COPY "Benchmark fused native CRC and payload copy" OFF)
    option(PF_SDIO_PIO_HIGH_SPEED "Native High-Speed startup (up to 50 MHz, limited by SYSCLK/4) with one 25 MHz fallback" ON)
    target_compile_definitions(pf_sdcard_protocol PRIVATE
        PF_SD_NATIVE_FUSED_COPY=$<BOOL:${PF_SDIO_PIO_FUSED_COPY}>)
    target_compile_definitions(pf_sdio_backend PRIVATE
        PF_SD_NATIVE_HIGH_SPEED=$<BOOL:${PF_SDIO_PIO_HIGH_SPEED}>
        PF_CONFIG_SDCARD_INTERRUPT_WAIT=$<BOOL:${PF_SDCARD_INTERRUPT_WAIT}>
    )
    if(PF_SDCARD_INTERRUPT_WAIT)
        target_sources(pf_sdio_backend PRIVATE
            src/drivers/sdcard/sdio_pio/sdio_wait_freertos.c)
        target_link_libraries(pf_sdio_backend PRIVATE FreeRTOS-Kernel-Static hardware_irq)
    endif()
    pico_generate_pio_header(pf_sdio_backend
        "${CMAKE_CURRENT_LIST_DIR}/src/drivers/sdcard/sdio_pio/sdio_pio.pio"
    )
    target_include_directories(pf_sdio_backend
        PUBLIC
            "${CMAKE_CURRENT_LIST_DIR}/src/drivers/sdcard"
    )
    target_link_libraries(pf_sdio_backend PUBLIC
        pf_sd_link
        pf_sdcard_protocol
        hardware_clocks
        hardware_dma
        hardware_gpio
        hardware_pio
        hardware_sync
        pico_stdlib
    )
    set(PF_SDCARD_BACKEND pf_sdio_backend)
else()
    add_library(pf_sd_spi_backend STATIC
        src/drivers/sdcard/sd_spi.c
        src/drivers/sdcard/spi/spi.c
    )
    target_include_directories(pf_sd_spi_backend PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/src/drivers/sdcard"
    )
    target_link_libraries(pf_sd_spi_backend PUBLIC
        pf_sd_link
        pf_sdcard_protocol
        hardware_spi
        hardware_gpio
        pico_stdlib
    )
    set(PF_SDCARD_BACKEND pf_sd_spi_backend)
endif()

add_library(pf_sdcard STATIC
    src/drivers/sdcard/sd.c
    src/drivers/sdcard/sd_board.c
)
target_include_directories(pf_sdcard PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers/sdcard"
)
target_link_libraries(pf_sdcard PUBLIC
    pf_sdcard_protocol
    ${PF_SDCARD_BACKEND}
    hardware_gpio
    pf_boot_config
)

if(PF_SDCARD_PORT STREQUAL "FREERTOS")
    target_sources(pf_sdcard PRIVATE
        src/drivers/sdcard/sd_port_freertos.c
    )
    target_compile_definitions(pf_sdcard PRIVATE
        PF_CONFIG_SDCARD_INTERRUPT_WAIT=$<BOOL:${PF_SDCARD_INTERRUPT_WAIT}>
    )
    target_link_libraries(pf_sdcard PRIVATE FreeRTOS-Kernel-Static hardware_irq)
else()
    target_sources(pf_sdcard PRIVATE
        src/drivers/sdcard/sd_port_baremetal.c
    )
    # The USB/storage application requires FreeRTOS; this target checks the
    # reusable driver without importing the kernel or accessing card hardware.
    add_executable(pf_sdcard_baremetal_smoke tests/ports/sdcard_baremetal_smoke.c)
    target_link_libraries(pf_sdcard_baremetal_smoke PRIVATE pf_sdcard)
    pico_enable_stdio_usb(pf_sdcard_baremetal_smoke 0)
    pico_enable_stdio_uart(pf_sdcard_baremetal_smoke 0)
    pico_set_program_version(pf_sdcard_baremetal_smoke "${PF_FIRMWARE_VERSION}")
    pico_add_extra_outputs(pf_sdcard_baremetal_smoke)
    return()
endif()

add_library(pf_system STATIC
    src/drivers/system.c
)
target_include_directories(pf_system PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers"
)
target_link_libraries(pf_system PUBLIC hardware_clocks hardware_vreg pico_stdlib)

add_library(pf_temperature STATIC
    src/drivers/temperature.c
)
target_include_directories(pf_temperature PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers"
)
target_link_libraries(pf_temperature PUBLIC hardware_adc)

add_library(pf_led STATIC
    src/drivers/led.c
)
target_include_directories(pf_led PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers"
)
target_link_libraries(pf_led PUBLIC hardware_gpio)

add_library(pf_monitor_uart STATIC
    src/drivers/monitor_uart.c
)
target_include_directories(pf_monitor_uart PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers"
)
target_link_libraries(pf_monitor_uart PUBLIC hardware_uart pico_stdio_uart)

add_library(pf_logging STATIC
    src/middleware/logging/logging.c
)
target_include_directories(pf_logging PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src"
)
target_link_libraries(pf_logging PUBLIC pico_stdlib)

add_library(embedded_cli STATIC
    third_party/embedded-cli/lib/src/embedded_cli.c
)
target_include_directories(embedded_cli PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/third_party/embedded-cli/lib/include"
)

add_library(pf_cli STATIC
    src/app/cli/cli.c
    src/app/cli/cli_storage.c
)
target_include_directories(pf_cli PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src"
)
target_link_libraries(pf_cli PUBLIC embedded_cli pf_storage)

add_library(pf_storage STATIC
    src/middleware/storage/block_device.c
    third_party/fatfs/source/ff.c
    third_party/fatfs/source/ffunicode.c
    src/middleware/storage/fatfs_disk.c
    src/middleware/storage/fatfs_volume.c
    src/middleware/storage/storage.c
)
target_link_libraries(pf_storage PUBLIC pf_sdcard pf_logging)
target_include_directories(pf_storage PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src/integration/fatfs"
    "${CMAKE_CURRENT_LIST_DIR}/src/middleware/storage"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/fatfs/source"
)

add_library(pf_app_storage STATIC
    src/app/storage/file_benchmark.c
    src/app/storage/storage_worker.c
)
option(PF_STORAGE_SELF_TEST_PREALLOCATE "Preallocate contiguous self-test files outside write timing" OFF)
target_compile_definitions(pf_app_storage PUBLIC
    PF_STORAGE_SELF_TEST_PREALLOCATE=$<BOOL:${PF_STORAGE_SELF_TEST_PREALLOCATE}>
)
target_include_directories(pf_app_storage PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src/app/storage"
)
target_link_libraries(pf_app_storage PUBLIC
    pf_cli
    pf_storage
    pf_sdcard
    pf_system
    embedded_cli
    FreeRTOS-Kernel-Static
)
add_library(pf_platform_boot_config_store STATIC
    src/platform/boot_config_store.c
)
target_include_directories(pf_platform_boot_config_store PUBLIC "${CMAKE_CURRENT_LIST_DIR}/src")
# pico_flash is an INTERFACE library: flash.c compiles into this target, so it needs the FreeRTOS SMP definitions
# (LIB_FREERTOS_KERNEL, FREE_RTOS_KERNEL_SMP, configNUMBER_OF_CORES, configUSE_CORE_AFFINITY) here.
target_link_libraries(pf_platform_boot_config_store PUBLIC
    hardware_flash
    pico_flash
    pico_sync
    FreeRTOS-Kernel-Static
)

add_library(pf_boot_config STATIC src/drivers/boot_config.c)
target_include_directories(pf_boot_config PUBLIC "${CMAKE_CURRENT_LIST_DIR}/src")
target_link_libraries(pf_boot_config PUBLIC pf_platform_boot_config_store)

target_link_libraries(pf_app_storage PUBLIC pf_logging)

# The board config is compiled into the image so its extern assignments always resolve for every driver library.
add_executable(pico_forge
    src/main.c
    src/config/board.c
    src/app/runtime_stats.c
    src/integration/usb/dcd_guard.c
    src/app/usb/usb_descriptors.c
    src/app/usb/usb_hid.c
    src/app/usb/usb_msc.c
    src/app/usb/usb_task.c
)

target_include_directories(pico_forge PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "${CMAKE_CURRENT_LIST_DIR}/src/integration/usb"
    "${CMAKE_CURRENT_LIST_DIR}/src/app/usb"
)

target_link_libraries(pico_forge
    pico_stdlib
    FreeRTOS-Kernel-Static
    hardware_irq
    pico_unique_id
    tinyusb_device
    pf_system
    pf_temperature
    pf_logging
    pf_cli
    pf_app_storage
    pf_led
    pf_monitor_uart
    pf_boot_config
)

target_compile_definitions(pico_forge PRIVATE
    PF_FIRMWARE_VERSION_MAJOR=${PF_FIRMWARE_VERSION_MAJOR}u
    PF_FIRMWARE_VERSION_MINOR=${PF_FIRMWARE_VERSION_MINOR}u
    PF_FIRMWARE_VERSION_PATCH=${PF_FIRMWARE_VERSION_PATCH}u
    PF_BCD_DEVICE=${PF_BCD_DEVICE}u
)

# Interpose TinyUSB MSC reset processing without modifying the pinned SDK.
target_link_options(pico_forge PRIVATE
    "LINKER:--wrap=mscd_reset"
    "LINKER:--wrap=mscd_control_xfer_cb"
)

# Keep the pinned DCD's endpoint state and fixed-core IRQ behind one cross-core gate.
foreach(symbol IN ITEMS
    dcd_init dcd_deinit dcd_int_enable dcd_int_disable dcd_set_address dcd_remote_wakeup
    dcd_disconnect dcd_connect dcd_sof_enable dcd_edpt0_status_complete dcd_edpt_open
    dcd_edpt_iso_alloc dcd_edpt_iso_activate dcd_edpt_close_all dcd_edpt_xfer
    dcd_edpt_stall dcd_edpt_clear_stall dcd_int_handler dcd_event_handler tud_task_ext tud_task_event_ready
    irq_add_shared_handler irq_remove_handler)
    target_link_options(pico_forge PRIVATE "LINKER:--wrap=${symbol}")
endforeach()

# Reserve the last flash sector for settings: shrink the linker FLASH region only. PICO_FLASH_SIZE_BYTES (the
# physical size used by the settings store) is left untouched, unlike pico_override_flash_size().
if(PICO_FLASH_SIZE_BYTES)
    set(pf_flash_size "${PICO_FLASH_SIZE_BYTES}")
elseif(PICO_DEFAULT_FLASH_SIZE_BYTES)
    set(pf_flash_size "${PICO_DEFAULT_FLASH_SIZE_BYTES}")
else()
    set(pf_flash_size "2 * 1024 * 1024")
endif()
set(PF_SETTINGS_SECTOR_SIZE 4096)
set(pf_link_dir "${CMAKE_CURRENT_BINARY_DIR}/pf_boot_config_link")
file(WRITE "${pf_link_dir}/pico_flash_region.ld"
    "FLASH(rx) : ORIGIN = 0x10000000, LENGTH = (${pf_flash_size}) - ${PF_SETTINGS_SECTOR_SIZE}\n")
pico_add_linker_script_override_path(pico_forge "${pf_link_dir}" FILES pico_flash_region.ld)
# Independent check of the linked image, run before the UF2/BIN/HEX outputs are generated.
add_custom_command(TARGET pico_forge POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -DPF_NM=${CMAKE_NM} -DPF_ELF=$<TARGET_FILE:pico_forge>
            "-DPF_FLASH_SIZE_BYTES=${pf_flash_size}" -DPF_SECTOR_SIZE=${PF_SETTINGS_SECTOR_SIZE}
            -P "${CMAKE_CURRENT_LIST_DIR}/check_settings_sector.cmake"
    VERBATIM)

pico_enable_stdio_usb(pico_forge 0)
pico_set_program_name(pico_forge "PicoForge")
pico_set_program_version(pico_forge "${PF_FIRMWARE_VERSION}")
pico_enable_stdio_uart(pico_forge 0)

include("${CMAKE_CURRENT_LIST_DIR}/artifact_name.cmake")
set_target_properties(pico_forge PROPERTIES OUTPUT_NAME "${PF_ARTIFACT_NAME}")
target_compile_definitions(pico_forge PRIVATE PF_IMAGE_SYS_CLK_MHZ=${pf_image_clock})
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/pico_forge_elf.txt"
    CONTENT "$<TARGET_FILE:pico_forge>\n")
pico_add_extra_outputs(pico_forge)