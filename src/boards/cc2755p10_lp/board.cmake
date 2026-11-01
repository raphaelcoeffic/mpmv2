# LP-EM-CC2755P10 (CC27xx) applications behind the boot loader, as on the
# MPM v2 (docs/update.md): the boot loader at 0 with our CCFG (ccfg.c: no
# protection, its CRCs filled in after the link), the applications at
# 0x2000, the SCFG on the device kept. Flashed with OpenOCD's cc27xx driver
# (github.com/raphaelcoeffic/openocd, cc27xx) over the XDS110:
# CC27_OPENOCD="<openocd-cc27xx>/src/openocd -s <openocd-cc27xx>/tcl" when
# not installed.

message("## Target is LP-EM-CC2755P10")

set(CC27_OPENOCD openocd CACHE STRING "OpenOCD with the cc27xx flash driver (and its arguments)")
separate_arguments(cc27_openocd UNIX_COMMAND "${CC27_OPENOCD}")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

include(platforms/cc27xx/platform.cmake)

# Boot loader in sectors 0 to 3 with the CCFG
add_board_elf(bootloader bootloader ${board_dir}/bootloader.ld
    core/bootloader.c ${platform_dir}/bl_port.c ${platform_dir}/board.c
    ${board_dir}/board.c ${board_dir}/ccfg.c)
target_compile_options(bootloader PRIVATE -Os)
add_custom_command(
    TARGET bootloader POST_BUILD
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/cc27_ccfg.py
            --objcopy ${CMAKE_OBJCOPY} bootloader.elf
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Target <name>: in the build directory <name>.elf, <name>.bin with the
# image header (no CRC, already checked) and <name>-update.bin for an
# STK500 update. flash-<name>: the boot loader, then the application at
# 0x2000, then a reset
function(add_cc27_application name)
    add_board_app(${name} ${board_dir}/app.ld ${ARGN} ${platform_dir}/board.c ${board_dir}/board.c
        core/app_header.c core/syscalls.c)
    add_update_image(${name} ${name}-update.bin)
    add_custom_target(${name} ALL
        DEPENDS ${CMAKE_BINARY_DIR}/${name}.bin ${CMAKE_BINARY_DIR}/${name}-update.bin bootloader)

    add_custom_target(flash-${name}
        COMMAND ${cc27_openocd} -f board/ti/lp-em-cc2755p10.cfg
                -c "program ${CMAKE_BINARY_DIR}/bootloader.elf verify"
                -c "program ${CMAKE_BINARY_DIR}/${name}.bin 0x2000 verify reset exit"
        DEPENDS ${name}
        USES_TERMINAL
        VERBATIM
    )
endfunction()

# Radio bench: bench.c's shell (docs/bench.md) on UART0 at 921600 baud,
# radio.h, timer.h and uart.h from the platform (platforms/cc27xx)
add_cc27_application(cc27-bench core/main_bench.c core/bench.c ${board_dir}/bench_port.c
    core/frame.c drivers/sx1281.c ${cc27xx_hal_sources} ${cc27xx_rf_sources})
target_link_libraries(cc27-bench_elf lrf_cc27xxx10)

# The MPM port: the radio's link on UART0 (100000 8E2, mpm_port.c), the
# protocols of the MPM v2 build, the EEPROM in RAM (eeprom.c)
add_cc27_application(cc27-mpm core/main_mpm.c ${board_dir}/mpm_port.c ${board_dir}/eeprom.c
    ${cc27xx_hal_sources} ${cc27xx_rf_sources} core/frame.c core/log.c ${mpm_sources} ${mpm_chip_sources})
target_link_libraries(cc27-mpm_elf lrf_cc27xxx10)
target_mpm(cc27-mpm_elf)
# Another module's ID (e.g. 0x2bf3be8e, an MPM v2's): its bound receivers
set(CC27_MODULE_ID "" CACHE STRING "MPM module ID of the CC27 port (empty: the chip's)")
if (CC27_MODULE_ID)
    target_compile_definitions(cc27-mpm_elf PRIVATE MPM_MODULE_ID=${CC27_MODULE_ID})
endif()

# cmake --build <dir> --target flash: the MPM port, through the XDS110
mpm_flash_method(flash_method debugger)
add_custom_target(flash)
add_dependencies(flash flash-cc27-mpm)
