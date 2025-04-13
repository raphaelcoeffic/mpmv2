
# Fetch all .c files in driverlib/
file(GLOB driverlib_c_sources driverlib/*.c)

set(driverlib_sources
    ${driverlib_c_sources}
)

set(startup_file
    startup_files/startup_gcc.c
)

add_library(
    driverlib_cc13x2_cc26x2
    OBJECT
    ${driverlib_sources}
    ${startup_file}
)

target_link_libraries(
    driverlib_cc13x2_cc26x2
    PUBLIC toolchain_gcc_m4f
)

target_include_directories(
    driverlib_cc13x2_cc26x2
    PUBLIC .
)

if (DRIVERLIB_NOROM)
    message("## Use compiled driverlib (NOROM)")
    target_compile_definitions(
        driverlib_cc13x2_cc26x2 PUBLIC
        DRIVERLIB_NOROM
    )
endif()
