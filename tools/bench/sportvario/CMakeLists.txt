cmake_minimum_required(VERSION 3.13)

# S.Port sensor emulator (main.c): a FrSky vario on a receiver's S.Port, one
# GPIO (PIO half duplex), for a Raspberry Pi Pico:
#   cmake -B build -G Ninja -DPICO_SDK_PATH=~/Src/pico-sdk
#   cmake --build build
include(pico_sdk_import.cmake)

project(sportvario C CXX ASM)
set(CMAKE_C_STANDARD 11)
if(NOT PICO_BOARD)
    set(PICO_BOARD pico)
endif()

pico_sdk_init()

add_executable(sportvario main.c)
pico_generate_pio_header(sportvario ${CMAKE_CURRENT_LIST_DIR}/sport.pio)

set(SPORT_PIN 2 CACHE STRING "S.Port line (the receiver's S.Port pin)")
set(POWER_PIN 3 CACHE STRING "receiver power switch, high = on (-1: none)")
target_compile_definitions(sportvario PRIVATE SPORT_PIN=${SPORT_PIN} POWER_PIN=${POWER_PIN})

target_compile_options(sportvario PRIVATE -Wall -Wextra)
target_link_libraries(sportvario pico_stdlib pico_bootrom hardware_pio hardware_clocks)

pico_enable_stdio_usb(sportvario 1)
pico_enable_stdio_uart(sportvario 0)
pico_add_extra_outputs(sportvario)
