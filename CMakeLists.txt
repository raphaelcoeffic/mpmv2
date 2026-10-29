cmake_minimum_required(VERSION 3.21)

set(CMAKE_COLOR_MAKEFILE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ARM GCC toolchain
include(${CMAKE_SOURCE_DIR}/cmake/toolchain/arm-none-eabi.cmake)

project(MPMv2 VERSION 0.4.0.0)

# Optimized unless asked otherwise (radio timing)
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Target board: BOARD_<name> for the sources
set(BOARDS MPM_V2 CC2652R1_LAUNCHXL XJT_LITE)
list(JOIN BOARDS ", " boards_text)
set(BOARD MPM_V2 CACHE STRING "Target board (${boards_text})")
set_property(CACHE BOARD PROPERTY STRINGS ${BOARDS})
if (NOT BOARD IN_LIST BOARDS)
  message(FATAL_ERROR "BOARD=${BOARD}: one of ${boards_text}")
endif()

option(DEBUG "Enable debug output (Segger RTT)" OFF)
option(USE_CCFG "Include MCU user configuration (CCFG)" ON)
option(FRSKY_CC26XX_PLAN "FrSky X/X2 on FrSky's CC26xx frequency plan (OFF: the CC2500 plan, FRSKY_FSCTRL0_OFFSET below)" ON)

add_subdirectory(lib)
add_subdirectory(src)
