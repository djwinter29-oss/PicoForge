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

project(picoforge_hello_world C CXX ASM)

pico_sdk_init()

set(FREERTOS_KERNEL_PATH "${FREERTOS_KERNEL_PATH_VALUE}" CACHE PATH "Path to the pinned FreeRTOS-Kernel checkout")
set(FREERTOS_CONFIG_FILE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/src/integration/freertos")
include("${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2040/FreeRTOS_Kernel_import.cmake")

add_library(picoforge_sdio_backend STATIC
    src/drivers/sdio/sdio_pio.c
    src/drivers/sdio/sdio_host.c
    src/drivers/sdio/sdio_protocol.c
)
pico_generate_pio_header(picoforge_sdio_backend
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers/sdio/sdio_host.pio"
)
target_include_directories(picoforge_sdio_backend
    PUBLIC "${CMAKE_CURRENT_LIST_DIR}/src/drivers/sdio"
)
target_link_libraries(picoforge_sdio_backend PUBLIC
    hardware_clocks
    hardware_dma
    hardware_gpio
    hardware_pio
    hardware_sync
    pico_stdlib
)

add_library(picoforge_system STATIC
    src/drivers/system.c
)
target_include_directories(picoforge_system PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers"
)
target_link_libraries(picoforge_system PUBLIC hardware_clocks pico_stdlib)

add_library(picoforge_temperature STATIC
    src/drivers/temperature.c
)
target_include_directories(picoforge_temperature PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers"
)
target_link_libraries(picoforge_temperature PUBLIC hardware_adc)

add_library(picoforge_led STATIC
    src/drivers/led.c
)
target_include_directories(picoforge_led PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers"
)
target_link_libraries(picoforge_led PUBLIC hardware_gpio)

add_library(picoforge_fatfs STATIC
    third_party/fatfs/source/ff.c
    third_party/fatfs/source/ffunicode.c
)
target_include_directories(picoforge_fatfs PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src/integration/fatfs"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/fatfs/source"
)

add_library(picoforge_storage STATIC
    src/integration/fatfs/diskio.c
    src/middleware/storage/storage.c
)
target_include_directories(picoforge_storage PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src/integration/fatfs"
    "${CMAKE_CURRENT_LIST_DIR}/src/middleware/storage"
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers/sdio"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/fatfs/source"
)
target_link_libraries(picoforge_storage PUBLIC
    picoforge_fatfs
    picoforge_sdio_backend
)

add_library(picoforge_board STATIC
    src/board/board.c
    src/board/board_topology.c
)
target_include_directories(picoforge_board PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/src/board"
    "${CMAKE_CURRENT_LIST_DIR}/src/drivers/sdio"
)
target_link_libraries(picoforge_board PUBLIC
    hardware_gpio
    hardware_pio
    picoforge_sdio_backend
)

add_executable(picoforge_hello_world
    src/main.c
    src/usb/usb_descriptors.c
    src/usb/hid.c
    src/usb/task.c
)

target_include_directories(picoforge_hello_world PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "${CMAKE_CURRENT_LIST_DIR}/src/integration/usb"
    "${CMAKE_CURRENT_LIST_DIR}/src/usb"
)

target_link_libraries(picoforge_hello_world
    pico_stdlib
    FreeRTOS-Kernel-Static
    pico_unique_id
    tinyusb_device
    picoforge_system
    picoforge_temperature
    picoforge_led
    picoforge_sdio_backend
    picoforge_storage
    picoforge_board
)

target_compile_definitions(picoforge_hello_world PRIVATE
    configNUMBER_OF_CORES=1
)

pico_enable_stdio_usb(picoforge_hello_world 0)
pico_enable_stdio_uart(picoforge_hello_world 0)

pico_add_extra_outputs(picoforge_hello_world)