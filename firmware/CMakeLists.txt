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
if(PF_SDCARD_TRANSPORT STREQUAL "SPI")
    if(PF_SDCARD_INTERRUPT_WAIT)
        message(FATAL_ERROR "PF_SDCARD_INTERRUPT_WAIT requires PF_SDCARD_TRANSPORT=SDIO_PIO")
    endif()
elseif(NOT PF_SDCARD_TRANSPORT STREQUAL "SDIO_PIO")
    message(FATAL_ERROR "PF_SDCARD_TRANSPORT must be SDIO_PIO or SPI")
endif()

if(DEFINED PF_SYS_CLK_MHZ)
    message(FATAL_ERROR
        "PF_SYS_CLK_MHZ has been removed; use saved clock profiles. Clear the old option with cmake -U PF_SYS_CLK_MHZ.")
endif()
if(DEFINED PF_SDIO_PIO_HIGH_SPEED)
    message(FATAL_ERROR
        "PF_SDIO_PIO_HIGH_SPEED has been removed; use saved speed profiles. Clear it with cmake -U PF_SDIO_PIO_HIGH_SPEED.")
endif()

set(PICO_MBEDTLS_CONFIG_FILE "pf_mbedtls_config.h")
pico_sdk_init()

set(PF_HID_BOARD_MODEL_VALUE 0)
if(PICO_BOARD STREQUAL "pico")
    set(PF_HID_BOARD_MODEL_VALUE 1)
elseif(PICO_BOARD STREQUAL "pico2")
    set(PF_HID_BOARD_MODEL_VALUE 2)
elseif(PICO_BOARD STREQUAL "pico2_w")
    set(PF_HID_BOARD_MODEL_VALUE 3)
elseif(PICO_BOARD STREQUAL "pico_w")
    set(PF_HID_BOARD_MODEL_VALUE 4)
endif()
add_compile_definitions(PF_HID_BOARD_MODEL=${PF_HID_BOARD_MODEL_VALUE})

add_library(pf_sd_link STATIC
    src/drivers/sdcard/sd_link.c)
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
        src/drivers/sdcard/sdio/sd_backend_sdio.c
        src/drivers/sdcard/sdio/sd_sdio_host.c
        src/drivers/sdcard/sdio/sdio_pio.c
    )
    option(PF_SDIO_PIO_FUSED_COPY "Benchmark fused native CRC and payload copy" OFF)
    target_compile_definitions(pf_sdcard_protocol PRIVATE
        PF_SD_NATIVE_FUSED_COPY=$<BOOL:${PF_SDIO_PIO_FUSED_COPY}>)
    target_compile_definitions(pf_sdio_backend PRIVATE
        PF_CONFIG_SDCARD_INTERRUPT_WAIT=$<BOOL:${PF_SDCARD_INTERRUPT_WAIT}>
    )
    if(PF_SDCARD_INTERRUPT_WAIT)
        target_sources(pf_sdio_backend PRIVATE
            src/drivers/sdcard/sdio/sdio_pio_wait_freertos.c)
        target_link_libraries(pf_sdio_backend PRIVATE FreeRTOS-Kernel-Static hardware_irq)
    endif()
    pico_generate_pio_header(pf_sdio_backend
        "${CMAKE_CURRENT_LIST_DIR}/src/drivers/sdcard/sdio/sdio_pio.pio"
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
    set(PF_SDCARD_BOARD_SOURCE src/drivers/sdcard/sdio/sd_board_sdio.c)
    set(PF_SDCARD_ASSIGNMENT_SOURCE src/config/board_sdio.c)
else()
    add_library(pf_sd_spi_backend STATIC
        src/drivers/sdcard/spi/sd_backend_spi.c
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
    set(PF_SDCARD_BOARD_SOURCE src/drivers/sdcard/spi/sd_board_spi.c)
    set(PF_SDCARD_ASSIGNMENT_SOURCE src/config/board_spi.c)
endif()

add_library(pf_sdcard STATIC
    src/drivers/sdcard/sd.c
    src/drivers/sdcard/sd_board.c
    ${PF_SDCARD_BOARD_SOURCE}
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
    src/platform/system.c
)
target_include_directories(pf_system PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src/platform"
)
target_link_libraries(pf_system PUBLIC hardware_clocks hardware_vreg pico_stdlib)
target_link_libraries(pf_system PRIVATE pf_boot_config)

add_library(pf_temperature STATIC
    src/platform/temperature.c
)
target_include_directories(pf_temperature PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src/platform"
)
target_link_libraries(pf_temperature PUBLIC hardware_adc)

add_library(pf_led STATIC
    src/platform/led.c
)
target_include_directories(pf_led PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src/platform"
)
target_link_libraries(pf_led PUBLIC hardware_gpio)

add_library(pf_logging STATIC
    src/platform/logging.c
)
target_include_directories(pf_logging PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "${CMAKE_CURRENT_LIST_DIR}/src/platform"
)
target_link_libraries(pf_logging PUBLIC hardware_uart pico_stdio_uart pico_stdlib)

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
    src/platform/storage/block_device.c
    third_party/fatfs/source/ff.c
    third_party/fatfs/source/ffunicode.c
    src/platform/storage/fatfs_disk.c
    src/platform/storage/fatfs_volume.c
    src/platform/storage/storage.c
)
target_link_libraries(pf_storage PUBLIC pf_sdcard pf_logging)
target_include_directories(pf_storage PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src/integration/fatfs"
    "${CMAKE_CURRENT_LIST_DIR}/src/platform/storage"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/fatfs/source"
)

option(PF_ENABLE_STORAGE_BENCHMARK "Include the on-device file verification and throughput benchmark" OFF)
add_library(pf_app_storage STATIC src/app/storage/storage_worker.c)
if(PF_ENABLE_STORAGE_BENCHMARK)
    target_sources(pf_app_storage PRIVATE src/app/storage/file_benchmark.c)
endif()
option(PF_STORAGE_SELF_TEST_PREALLOCATE "Preallocate contiguous self-test files outside write timing" OFF)
target_compile_definitions(pf_app_storage PUBLIC
    PF_ENABLE_STORAGE_BENCHMARK=$<BOOL:${PF_ENABLE_STORAGE_BENCHMARK}>
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
add_library(pf_boot_config STATIC src/platform/boot_config.c)
target_include_directories(pf_boot_config PUBLIC "${CMAKE_CURRENT_LIST_DIR}/src")
# pico_flash is an INTERFACE library: flash.c compiles into this target, so it needs the FreeRTOS SMP definitions
# (LIB_FREERTOS_KERNEL, FREE_RTOS_KERNEL_SMP, configNUMBER_OF_CORES, configUSE_CORE_AFFINITY) here.
target_link_libraries(pf_boot_config PRIVATE
    hardware_flash
    pico_flash
    pico_sync
    FreeRTOS-Kernel-Static
)

# The board config is compiled into the image so its extern assignments always resolve for every driver library.
add_executable(pico_forge
    src/main.c
    src/config/board.c
    ${PF_SDCARD_ASSIGNMENT_SOURCE}
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
    pf_boot_config
)

if(PICO_CYW43_SUPPORTED)
    target_sources(pico_forge PRIVATE src/app/wireless/wireless.c src/app/wireless/wireless_auth.c)
    target_include_directories(pico_forge PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/integration/wireless")
    target_compile_definitions(pico_forge PRIVATE PF_ENABLE_WIRELESS=1)
    target_link_libraries(pico_forge pico_cyw43_arch_lwip_poll pico_btstack_ble pico_btstack_cyw43
                          pico_mbedtls pico_rand)
    pico_btstack_make_gatt_header(pico_forge PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/app/wireless/picoforge.gatt")
endif()

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