cmake_minimum_required(VERSION 3.13)

# S.BUS2 monitor and sensor emulator (main.c), for a Raspberry Pi Pico:
#   cmake -B build -G Ninja -DPICO_SDK_PATH=~/Src/pico-sdk
#   cmake --build build
include(pico_sdk_import.cmake)

project(sbus2mon C CXX ASM)
set(CMAKE_C_STANDARD 11)
if(NOT PICO_BOARD)
    set(PICO_BOARD pico)
endif()

pico_sdk_init()

add_executable(sbus2mon main.c)
pico_generate_pio_header(sbus2mon ${CMAKE_CURRENT_LIST_DIR}/sbus_rx.pio)
pico_generate_pio_header(sbus2mon ${CMAKE_CURRENT_LIST_DIR}/sbus_tx.pio)

set(SBUS2_PIN 2 CACHE STRING "the receiver's S.BUS2 line")
target_compile_definitions(sbus2mon PRIVATE SBUS2_PIN=${SBUS2_PIN})

target_compile_options(sbus2mon PRIVATE -Wall -Wextra)
target_link_libraries(sbus2mon pico_stdlib pico_multicore pico_util hardware_pio hardware_clocks)

pico_enable_stdio_usb(sbus2mon 1)
pico_enable_stdio_uart(sbus2mon 0)
pico_add_extra_outputs(sbus2mon)
