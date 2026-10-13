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

project(pf_hello_world C CXX ASM)

pico_sdk_init()

set(FREERTOS_KERNEL_PATH "${FREERTOS_KERNEL_PATH_VALUE}" CACHE PATH "Path to the pinned FreeRTOS-Kernel checkout")
set(FREERTOS_CONFIG_FILE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/src/integration/freertos")
include("${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2040/FreeRTOS_Kernel_import.cmake")

add_library(pf_sd_card_protocol STATIC
    src/drivers/sd_card/sd_card_protocol.c
)
target_include_directories(pf_sd_card_protocol PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers/sd_card"
)

add_library(pf_sdio_backend STATIC
    src/drivers/sd_card/sdio_pio/sdio_pio.c
    src/drivers/sd_card/sdio_pio/sdio_pio_host.c
    src/drivers/sd_card/sdio_pio/sdio_pio_protocol.c
)
pico_generate_pio_header(pf_sdio_backend
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers/sd_card/sdio_pio/sdio_pio_host.pio"
)
target_include_directories(pf_sdio_backend
    PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/src/drivers/sd_card"
        "${CMAKE_CURRENT_LIST_DIR}/src/drivers/sd_card/sdio_pio"
)
target_link_libraries(pf_sdio_backend PUBLIC
    pf_sd_card_protocol
    hardware_clocks
    hardware_dma
    hardware_gpio
    hardware_pio
    hardware_sync
    pico_stdlib
)

add_library(pf_sd_spi_backend STATIC
    src/drivers/sd_card/spi/sd_spi.c
    src/drivers/sd_card/spi/sd_spi_protocol.c
)
target_include_directories(pf_sd_spi_backend PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers/sd_card/spi"
)
target_link_libraries(pf_sd_spi_backend PUBLIC
    pf_sd_card_protocol
    hardware_spi
    hardware_gpio
    pico_stdlib
)

add_library(pf_sd_card STATIC
    src/drivers/sd_card/sd_card.c
)
target_include_directories(pf_sd_card PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers/sd_card"
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers/sd_card/sdio_pio"
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers/sd_card/spi"
)
target_link_libraries(pf_sd_card PUBLIC
    pf_sd_card_protocol
    pf_sd_spi_backend
    pf_sdio_backend
)

add_library(pf_system STATIC
    src/drivers/system.c
)
target_include_directories(pf_system PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers"
)
target_link_libraries(pf_system PUBLIC hardware_clocks pico_stdlib)

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
    src/middleware/cli/cli.c
)
target_include_directories(pf_cli PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src"
)
target_link_libraries(pf_cli PUBLIC embedded_cli)

add_library(pf_storage STATIC
    third_party/fatfs/source/ff.c
    third_party/fatfs/source/ffunicode.c
    src/integration/fatfs/diskio.c
    src/middleware/storage/storage.c
    src/middleware/storage/commands.c
    src/middleware/storage/self_test.c
)
target_include_directories(pf_storage PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src/integration/fatfs"
    "${CMAKE_CURRENT_LIST_DIR}/src/middleware/storage"
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers/sd_card"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/fatfs/source"
)
target_link_libraries(pf_storage PUBLIC
    pf_sd_card
    embedded_cli
)

add_library(pf_board STATIC
    src/board/board.c
    src/board/board_profile.c
    src/board/board_resources.c
)
target_include_directories(pf_board PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "${CMAKE_CURRENT_LIST_DIR}/src/board"
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers/sd_card"
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers/sd_card/sdio_pio"
)
target_link_libraries(pf_board PUBLIC
    hardware_gpio
    hardware_pio
    hardware_spi
    pico_stdio_uart
    pf_led
    pf_sd_spi_backend
    pf_sdio_backend
)

add_executable(pf_hello_world
    src/main.c
    src/usb/usb_descriptors.c
    src/usb/hid.c
    src/usb/msc.c
    src/usb/task.c
    src/middleware/storage/storage_worker.c
)

target_include_directories(pf_hello_world PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "${CMAKE_CURRENT_LIST_DIR}/src/integration/usb"
    "${CMAKE_CURRENT_LIST_DIR}/src/usb"
)

target_link_libraries(pf_hello_world
    pico_stdlib
    FreeRTOS-Kernel-Static
    pico_unique_id
    tinyusb_device
    pf_system
    pf_temperature
    pf_logging
    pf_cli
    pf_sdio_backend
    pf_storage
    pf_board
)

target_compile_definitions(pf_hello_world PRIVATE
    configNUMBER_OF_CORES=1
)

pico_enable_stdio_usb(pf_hello_world 0)
pico_enable_stdio_uart(pf_hello_world 0)

pico_add_extra_outputs(pf_hello_world)