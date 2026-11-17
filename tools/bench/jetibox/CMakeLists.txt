cmake_minimum_required(VERSION 3.13)

# JETIBOX emulator (main.c), for a Raspberry Pi Pico:
#   cmake -B build -G Ninja -DPICO_SDK_PATH=~/Src/pico-sdk
#   cmake --build build
include(pico_sdk_import.cmake)

project(jetibox C CXX ASM)
set(CMAKE_C_STANDARD 11)
if(NOT PICO_BOARD)
    set(PICO_BOARD pico)
endif()

pico_sdk_init()

add_executable(jetibox main.c)
pico_generate_pio_header(jetibox ${CMAKE_CURRENT_LIST_DIR}/jbox_rx.pio)
pico_generate_pio_header(jetibox ${CMAKE_CURRENT_LIST_DIR}/jbox_tx.pio)
pico_generate_pio_header(jetibox ${CMAKE_CURRENT_LIST_DIR}/pulse.pio)
pico_generate_pio_header(jetibox ${CMAKE_CURRENT_LIST_DIR}/exbus_rx.pio)
pico_generate_pio_header(jetibox ${CMAKE_CURRENT_LIST_DIR}/exbus_tx.pio)

set(JBOX_PIN 2 CACHE STRING "the device's JETIBOX line (receiver Ext)")
set(PULSE_PIN 3 CACHE STRING "servo pulse meter input")
target_compile_definitions(jetibox PRIVATE JBOX_PIN=${JBOX_PIN} PULSE_PIN=${PULSE_PIN})

target_compile_options(jetibox PRIVATE -Wall -Wextra)
target_link_libraries(jetibox pico_stdlib pico_multicore pico_util hardware_pio hardware_clocks)

pico_enable_stdio_usb(jetibox 1)
pico_enable_stdio_uart(jetibox 0)
pico_add_extra_outputs(jetibox)
