cmake_minimum_required(VERSION 3.21)

set(CMAKE_COLOR_MAKEFILE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ARM GCC toolchain
include(${CMAKE_SOURCE_DIR}/cmake/toolchain/arm-none-eabi.cmake)

project(MPMv2 VERSION 0.2.0.0)

# Optimized unless asked otherwise (radio timing)
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Target board: BOARD_<name> for the sources
set(BOARDS MPM_V2 CC2652R1_LAUNCHXL)
list(JOIN BOARDS ", " boards_text)
set(BOARD MPM_V2 CACHE STRING "Target board (${boards_text})")
set_property(CACHE BOARD PROPERTY STRINGS ${BOARDS})
if (NOT BOARD IN_LIST BOARDS)
  message(FATAL_ERROR "BOARD=${BOARD}: one of ${boards_text}")
endif()

option(DEBUG "Enable debug output (Segger RTT)" OFF)
option(USE_CCFG "Include MCU user configuration (CCFG)" ON)
option(BENCH "Radio bench mode: test shell on UART0 instead of the MPM" OFF)
option(BOOTLOADER "Boot loader at 0x0 (update from the radio), application at 0x2000" OFF)

add_subdirectory(lib)
add_subdirectory(src)
