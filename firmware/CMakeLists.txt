cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(DEFINED PICO_SDK_PATH)
    include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
elseif(DEFINED ENV{PICO_SDK_PATH})
    include("$ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake")
else()
    message(FATAL_ERROR "Set PICO_SDK_PATH to your Pico SDK checkout before configuring firmware.")
endif()

project(picoforge_hello_world C CXX ASM)

pico_sdk_init()

add_executable(picoforge_hello_world
    src/main.c
)

target_link_libraries(picoforge_hello_world
    pico_stdlib
)

pico_enable_stdio_usb(picoforge_hello_world 1)
pico_enable_stdio_uart(picoforge_hello_world 0)

pico_add_extra_outputs(picoforge_hello_world)