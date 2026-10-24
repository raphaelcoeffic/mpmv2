# CC26x0 driverlib: TI's CC26xxware (BSD-3-Clause), from Contiki-NG's copy
# (github.com/contiki-ng/cc26xxware fffef6e, driverlib build 47020). The RF
# patches are headers only (rf_patches/), rf_patch_cpe_genfsk.h from the
# SimpleLink CC13xx/CC26xx SDK's cc26x0 folder (Contiki-NG's coresdk_cc13xx_cc26xx,
# revision 18438, 2018: TI's latest, the
# same MCE and RFE patches). Changes: startup_gcc.c (section
# and symbols of src/linker/sections.ld; MSP, CONTROL, BASEPRI, VTOR and interrupts
# for an image behind another boot loader), interrupt.c (RAM vector table in .ramVecs)

file(GLOB driverlib_c_sources driverlib/*.c)

add_library(
    driverlib_cc26x0
    OBJECT
    ${driverlib_c_sources}
    startup_files/startup_gcc.c
)

target_link_libraries(
    driverlib_cc26x0
    PUBLIC toolchain_gcc_m3
)

target_include_directories(
    driverlib_cc26x0
    PUBLIC .
)

if (DRIVERLIB_NOROM)
    target_compile_definitions(
        driverlib_cc26x0 PUBLIC
        DRIVERLIB_NOROM
    )
endif()
