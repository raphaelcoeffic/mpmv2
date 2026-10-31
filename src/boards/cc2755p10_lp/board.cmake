# LP-EM-CC2755P10 (CC27xx) applications behind the boot loader, as on the
# MPM v2 (docs/update.md): the boot loader at 0 with our CCFG (ccfg.c: no
# protection, its CRCs filled in after the link), the applications at
# 0x2000, the SCFG on the device kept. Flashed with OpenOCD's cc27xx driver
# (github.com/raphaelcoeffic/openocd, cc27xx) over the XDS110:
# CC27_OPENOCD="<openocd-cc27xx>/src/openocd -s <openocd-cc27xx>/tcl" when
# not installed.

message("## Target is LP-EM-CC2755P10: radio bring-up")

set(CC27_OPENOCD openocd CACHE STRING "OpenOCD with the cc27xx flash driver (and its arguments)")
separate_arguments(cc27_openocd UNIX_COMMAND "${CC27_OPENOCD}")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

include(platforms/cc27xx/platform.cmake)

# Boot loader in sectors 0 to 3 with the CCFG
add_executable(bootloader core/bootloader.c ${cc27xx_dir}/bl_port.c ${cc27xx_dir}/board.c
    ${board_dir}/board.c ${board_dir}/ccfg.c)
target_compile_definitions(bootloader PRIVATE ${board_definitions})
target_compile_options(bootloader PRIVATE -Os)
target_include_directories(bootloader PRIVATE ${board_dir} ${cc27xx_dir} ${src_include_dirs})
target_link_libraries(bootloader driverlib_cc27xxx10)
target_link_options(bootloader PRIVATE -L${CMAKE_CURRENT_SOURCE_DIR}/linker)
set_target_properties(bootloader PROPERTIES
    LINKER_FILE ${board_dir}/bootloader.ld
    # Relinked when a script changes
    LINK_DEPENDS "${board_dir}/bootloader.ld;${CMAKE_CURRENT_SOURCE_DIR}/linker/sections.ld"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
add_custom_command(
    TARGET bootloader POST_BUILD
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/cc27_ccfg.py
            --objcopy ${CMAKE_OBJCOPY} bootloader.elf
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Target <name>: in the build directory <name>.elf, <name>.bin with the
# image header (no CRC, already checked) and <name>-update.bin for an
# STK500 update (tools/fw_image.py: CRC, MULTI signature). flash-<name>:
# the boot loader, then the application at 0x2000, then a reset
function(add_cc27_application name)
    set(elf ${name}_elf)
    add_executable(${elf} ${ARGN} ${cc27xx_dir}/board.c ${board_dir}/board.c
        core/app_header.c core/syscalls.c)
    target_include_directories(${elf} PRIVATE ${board_dir} ${cc27xx_dir} ${src_include_dirs})
    target_compile_definitions(${elf} PRIVATE ${board_definitions})
    target_link_libraries(${elf} driverlib_cc27xxx10)
    target_link_options(${elf} PRIVATE -L${CMAKE_CURRENT_SOURCE_DIR}/linker)
    set_target_properties(${elf} PROPERTIES
        OUTPUT_NAME ${name}
        LINKER_FILE ${board_dir}/app.ld
        # Relinked when a script changes
        LINK_DEPENDS "${board_dir}/app.ld;${CMAKE_CURRENT_SOURCE_DIR}/linker/sections.ld"
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )

    set(images ${name}.bin ${name}-update.bin)
    list(TRANSFORM images PREPEND ${CMAKE_BINARY_DIR}/)
    add_custom_command(
        OUTPUT ${images}
        COMMAND ${CMAKE_OBJCOPY} -O binary ${name}.elf ${name}.bin
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/fw_image.py ${name}.bin
                --version ${PROJECT_VERSION} --out ${name}-update.bin
        DEPENDS ${elf} ${CMAKE_SOURCE_DIR}/tools/fw_image.py
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    add_custom_target(${name} ALL DEPENDS ${images} bootloader)

    add_custom_target(flash-${name}
        COMMAND ${cc27_openocd} -f board/ti/lp-em-cc2755p10.cfg
                -c "program ${CMAKE_BINARY_DIR}/bootloader.elf verify"
                -c "program ${CMAKE_BINARY_DIR}/${name}.bin 0x2000 verify reset exit"
        DEPENDS ${name}
        USES_TERMINAL
        VERBATIM
    )
endfunction()

# Bring-up: UART0, LEDs, buttons, HFXT
add_cc27_application(cc27-test ${board_dir}/test.c)

# Radio bring-up: a shell on UART0 driving the radio (TI's NESB PHY, raw
# framing). The PHY files come from tools/cc27_phy.py nesb_1m_2m_cc27xx
add_cc27_application(cc27-radio ${board_dir}/radio_test.c ${cc27xx_rf_sources}
    ${cc27xx_dir}/phy_ble.c)
target_link_libraries(cc27-radio_elf lrf_cc27xxx10)

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
