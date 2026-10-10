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

add_executable(picoforge_hello_world
    src/main.c
    src/integration/usb/usb_descriptors.c
)

target_include_directories(picoforge_hello_world PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/src/integration/usb"
)

target_link_libraries(picoforge_hello_world
    pico_stdlib
    FreeRTOS-Kernel-Static
    pico_unique_id
    tinyusb_device
)

target_compile_definitions(picoforge_hello_world PRIVATE
    configNUMBER_OF_CORES=1
)

pico_enable_stdio_usb(picoforge_hello_world 0)
pico_enable_stdio_uart(picoforge_hello_world 0)

pico_add_extra_outputs(picoforge_hello_world)