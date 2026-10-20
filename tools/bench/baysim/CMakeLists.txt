cmake_minimum_required(VERSION 3.13)

# Module bay simulator (main.c), for an RP2040-Zero:
#   cmake -B build -G Ninja -DPICO_SDK_PATH=~/Src/pico-sdk
#   cmake --build build
include(pico_sdk_import.cmake)

project(baysim C CXX ASM)
set(CMAKE_C_STANDARD 11)
if(NOT PICO_BOARD)
    set(PICO_BOARD waveshare_rp2040_zero)
endif()

pico_sdk_init()

add_executable(baysim main.c usb_descriptors.c)
target_include_directories(baysim PRIVATE ${CMAKE_CURRENT_LIST_DIR})

# Pins: the bench's RP2040-Zero (GP11 power switch, GP12 / GP13 the bay lines)
set(LINK_UART   0  CACHE STRING "UART instance for the bay lines")
set(LINK_TX_PIN 12 CACHE STRING "UART TX pin, to the bay's PPM pin")
set(LINK_RX_PIN 13 CACHE STRING "UART RX pin, from the bay's S.Port pin")
set(POWER_PIN   11 CACHE STRING "module power switch, high = on")
target_compile_definitions(baysim PRIVATE
    LINK_UART=${LINK_UART}
    LINK_TX_PIN=${LINK_TX_PIN}
    LINK_RX_PIN=${LINK_RX_PIN}
    POWER_PIN=${POWER_PIN}
)

target_compile_options(baysim PRIVATE -Wall -Wextra)
target_link_libraries(baysim pico_stdlib pico_unique_id pico_bootrom hardware_uart hardware_irq tinyusb_device)

pico_enable_stdio_usb(baysim 0)
pico_enable_stdio_uart(baysim 0)
pico_add_extra_outputs(baysim)
