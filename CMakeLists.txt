cmake_minimum_required(VERSION 3.21)

set(CMAKE_COLOR_MAKEFILE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ARM GCC toolchain
include(${CMAKE_SOURCE_DIR}/cmake/toolchain/arm-none-eabi.cmake)

project(MPMv2)

option(BOARD_CC2652R1_LAUNCHXL
  "Target board is CC2652R1-LAUNCHXL (otherwise MPM w/ CC2652RB is assumed)"
  OFF
)

option(DEBUG "Enable debug output (Segger RTT)" OFF)
option(USE_CCFG "Include MCU user configuration (CCFG)" ON)
option(USE_XOSC "Use external oscillator (or HPOSC on CC2652RB)" ON)

add_subdirectory(lib/ti)
add_subdirectory(lib/SeggerRTT)
add_subdirectory(src)
