# CC27xx (CC2755P10) driverlib: TI's SimpleLink Low Power F3 SDK
# (BSD-3-Clause), github.com/TexasInstruments/simplelink-lowpower-f3-sdk
# 4aa2c92 (2026-10-12), source/ti/devices/cc27xxx10: inc/, cmsis/ (no SVD),
# driverlib/ (without the TF-M veneers, the AES/SHA and the sw_* code;
# sha256sw.h kept for hapi.h), startup_files/startup_gcc.c (unchanged: the
# section and symbols of src/linker/sections.ld), and in rf_patches/ the LRF
# images the generic PHYs load (PBE generic, MCE/RFE genfsk, the common ones)

file(GLOB driverlib_c_sources driverlib/*.c)
file(GLOB rf_patches_sources rf_patches/*.c)

add_library(
    driverlib_cc27xxx10
    OBJECT
    ${driverlib_c_sources}
    ${rf_patches_sources}
    startup_files/startup_gcc.c
)

target_link_libraries(
    driverlib_cc27xxx10
    PUBLIC toolchain_gcc_m33f
)

target_include_directories(
    driverlib_cc27xxx10
    PUBLIC .
)
