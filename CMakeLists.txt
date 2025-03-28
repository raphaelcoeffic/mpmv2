cmake_minimum_required(VERSION 3.21)

set(CMAKE_COLOR_MAKEFILE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ARM GCC toolchain
include(${CMAKE_SOURCE_DIR}/cmake/toolchain/arm-none-eabi.cmake)

project(MPMv2)

add_subdirectory(lib/ti)
add_subdirectory(src)
