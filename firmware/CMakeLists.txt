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

set(PF_SD_CARD_TRANSPORT "SDIO_PIO" CACHE STRING "SD-card transport: SDIO_PIO or SPI")
set_property(CACHE PF_SD_CARD_TRANSPORT PROPERTY STRINGS SDIO_PIO SPI)
option(PF_SD_CARD_INTERRUPT_WAIT "Enable experimental FreeRTOS native PIO completion waits" OFF)
if(PF_SD_CARD_TRANSPORT STREQUAL "SDIO_PIO")
    set(PF_CONFIG_SD_CARD_TRANSPORT_VALUE 0)
elseif(PF_SD_CARD_TRANSPORT STREQUAL "SPI")
    set(PF_CONFIG_SD_CARD_TRANSPORT_VALUE 1)
    if(PF_SD_CARD_INTERRUPT_WAIT)
        message(FATAL_ERROR "PF_SD_CARD_INTERRUPT_WAIT requires PF_SD_CARD_TRANSPORT=SDIO_PIO")
    endif()
else()
    message(FATAL_ERROR "PF_SD_CARD_TRANSPORT must be SDIO_PIO or SPI")
endif()
add_compile_definitions(PF_CONFIG_SD_CARD_TRANSPORT=${PF_CONFIG_SD_CARD_TRANSPORT_VALUE})

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

set(FREERTOS_KERNEL_PATH "${FREERTOS_KERNEL_PATH_VALUE}" CACHE PATH "Path to the pinned FreeRTOS-Kernel checkout")
set(FREERTOS_CONFIG_FILE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/src/integration/freertos")
include("${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2040/FreeRTOS_Kernel_import.cmake")

add_library(pf_sd_card_protocol STATIC
    src/drivers/sd_card/sd_card_protocol.c
)
target_include_directories(pf_sd_card_protocol PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers/sd_card"
)

if(PF_SD_CARD_TRANSPORT STREQUAL "SDIO_PIO")
    add_library(pf_sdio_backend STATIC
        src/drivers/sd_card/sdio_pio/sdio_pio.c
        src/drivers/sd_card/sdio_pio/sdio_pio_host.c
        src/drivers/sd_card/sdio_pio/sdio_pio_protocol.c
    )
    option(PF_SDIO_PIO_FUSED_COPY "Benchmark fused native CRC and payload copy" OFF)
    target_compile_definitions(pf_sdio_backend PRIVATE
        PF_SDIO_PIO_FUSED_COPY=$<BOOL:${PF_SDIO_PIO_FUSED_COPY}>
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
    set(PF_SD_CARD_BACKEND pf_sdio_backend)
else()
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
    set(PF_SD_CARD_BACKEND pf_sd_spi_backend)
endif()

add_library(pf_sd_card STATIC
    src/drivers/sd_card/sd_card.c
)
target_include_directories(pf_sd_card PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers/sd_card"
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers/sd_card/sdio_pio"
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers/sd_card/spi"
)
target_link_libraries(pf_sd_card PUBLIC
    FreeRTOS-Kernel-Static
    pf_sd_card_protocol
    ${PF_SD_CARD_BACKEND}
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
option(PF_STORAGE_SELF_TEST_PREALLOCATE "Preallocate contiguous self-test files outside write timing" OFF)
target_compile_definitions(pf_storage PUBLIC
    PF_STORAGE_SELF_TEST_PREALLOCATE=$<BOOL:${PF_STORAGE_SELF_TEST_PREALLOCATE}>
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
    ${PF_SD_CARD_BACKEND}
)

add_executable(pf_hello_world
    src/main.c
    src/usb/usb_descriptors.c
    src/usb/hid.c
    src/usb/msc.c
    src/usb/task.c
    src/middleware/storage/storage_worker.c
    src/drivers/sd_card/freertos/sd_card_wait.c
)

target_compile_definitions(pf_hello_world PRIVATE
    PF_CONFIG_SD_CARD_INTERRUPT_WAIT=$<BOOL:${PF_SD_CARD_INTERRUPT_WAIT}>
)

target_include_directories(pf_hello_world PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "${CMAKE_CURRENT_LIST_DIR}/src/integration/usb"
    "${CMAKE_CURRENT_LIST_DIR}/src/usb"
)

target_link_libraries(pf_hello_world
    pico_stdlib
    FreeRTOS-Kernel-Static
    hardware_irq
    pico_unique_id
    tinyusb_device
    pf_system
    pf_temperature
    pf_logging
    pf_cli
    pf_storage
    pf_board
)

target_compile_definitions(pf_hello_world PRIVATE
    configNUMBER_OF_CORES=1
)

pico_enable_stdio_usb(pf_hello_world 0)
pico_enable_stdio_uart(pf_hello_world 0)

pico_add_extra_outputs(pf_hello_world)