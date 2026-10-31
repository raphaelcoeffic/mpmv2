# LP-EM-CC2755P10 (CC27xx) bring-up applications: no boot loader, the image
# at 0 with our CCFG (ccfg.c: no protection, its CRCs filled in after
# the link), the SCFG on the device kept. Flashed with OpenOCD's cc27xx
# driver (github.com/raphaelcoeffic/openocd, cc27xx) over the XDS110:
# CC27_OPENOCD="<openocd-cc27xx>/src/openocd -s <openocd-cc27xx>/tcl" when
# not installed.

set(CC27_OPENOCD openocd CACHE STRING "OpenOCD with the cc27xx flash driver (and its arguments)")
separate_arguments(cc27_openocd UNIX_COMMAND "${CC27_OPENOCD}")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(board_dir ${CMAKE_CURRENT_LIST_DIR})
include(platforms/cc27xx/platform.cmake)

# Target <name>: <name>.elf in the build directory. flash-<name>: the image
# and the CCFG, then a reset
function(add_cc27_application name)
    set(elf ${name}_elf)
    add_executable(${elf} ${ARGN} ${board_dir}/board.c ${board_dir}/ccfg.c syscalls.c)
    target_include_directories(${elf} PRIVATE ${board_dir} ${cc27xx_dir} ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${elf} PRIVATE ${board_definitions})
    target_link_libraries(${elf} driverlib_cc27xxx10)
    target_link_options(${elf} PRIVATE -L${CMAKE_CURRENT_SOURCE_DIR}/linker)
    set_target_properties(${elf} PROPERTIES
        OUTPUT_NAME ${name}
        LINKER_FILE ${board_dir}/cc27xx.ld
        # Relinked when a script changes
        LINK_DEPENDS "${board_dir}/cc27xx.ld;${CMAKE_CURRENT_SOURCE_DIR}/linker/sections.ld"
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    add_custom_command(
        TARGET ${elf} POST_BUILD
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/cc27_ccfg.py
                --objcopy ${CMAKE_OBJCOPY} ${name}.elf
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )

    add_custom_target(flash-${name}
        COMMAND ${cc27_openocd} -f board/ti/lp-em-cc2755p10.cfg
                -c "program ${CMAKE_BINARY_DIR}/${name}.elf verify reset exit"
        DEPENDS ${elf}
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
add_cc27_application(cc27-bench ${board_dir}/main.c bench.c frame.c sx1281.c
    ${cc27xx_hal_sources} ${cc27xx_rf_sources})
target_compile_definitions(cc27-bench_elf PRIVATE BENCH)
target_link_libraries(cc27-bench_elf lrf_cc27xxx10)

# The MPM port (main_mpm.c): the radio's link on UART0 (100000 8E2,
# mpm_port.c), the protocols of the MPM v2 build
add_cc27_application(cc27-mpm ${board_dir}/main_mpm.c ${board_dir}/mpm_port.c ${board_dir}/eeprom.c
    ${cc27xx_hal_sources} ${cc27xx_rf_sources} frame.c log.c ${mpm_sources})
target_link_libraries(cc27-mpm_elf lrf_cc27xxx10)
target_mpm_definitions(cc27-mpm_elf)
# Another module's ID (e.g. 0x2bf3be8e, an MPM v2's): its bound receivers
set(CC27_MODULE_ID "" CACHE STRING "MPM module ID of the CC27 port (empty: the chip's)")
if (CC27_MODULE_ID)
    target_compile_definitions(cc27-mpm_elf PRIVATE MPM_MODULE_ID=${CC27_MODULE_ID})
endif()
