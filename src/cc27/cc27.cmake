# LP-EM-CC2755P10 (CC27xx) bring-up applications: no boot loader, the image
# at 0 with our CCFG (cc27/ccfg.c: no protection, its CRCs filled in after
# the link), the SCFG on the device kept. Flashed with OpenOCD's cc27xx
# driver (github.com/raphaelcoeffic/openocd, cc27xx) over the XDS110:
# CC27_OPENOCD="<openocd-cc27xx>/src/openocd -s <openocd-cc27xx>/tcl" when
# not installed.

set(CC27_OPENOCD openocd CACHE STRING "OpenOCD with the cc27xx flash driver (and its arguments)")
separate_arguments(cc27_openocd UNIX_COMMAND "${CC27_OPENOCD}")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Target <name>: <name>.elf in the build directory. flash-<name>: the image
# and the CCFG, then a reset
function(add_cc27_application name)
    set(elf ${name}_elf)
    add_executable(${elf} ${ARGN} cc27/board.c cc27/ccfg.c syscalls.c)
    target_include_directories(${elf} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/cc27)
    target_compile_definitions(${elf} PRIVATE ${board_definitions})
    target_link_libraries(${elf} driverlib_cc27xxx10)
    target_link_options(${elf} PRIVATE -L${CMAKE_CURRENT_SOURCE_DIR}/linker)
    set_target_properties(${elf} PROPERTIES
        OUTPUT_NAME ${name}
        LINKER_FILE ${CMAKE_CURRENT_SOURCE_DIR}/linker/cc27xx.ld
        # Relinked when a script changes
        LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/linker/cc27xx.ld;${CMAKE_CURRENT_SOURCE_DIR}/linker/sections.ld"
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
add_cc27_application(cc27-test cc27/test.c)

# Radio bring-up: a shell on UART0 driving the radio (TI's NESB PHY, raw
# framing). The PHY files come from tools/cc27_phy.py nesb_1m_2m_cc27xx
add_cc27_application(cc27-radio cc27/radio_test.c cc27/rf.c cc27/lrf_hal.c cc27/phy_nesb.c cc27/phy_ble.c
    cc27/phy_fsk250.c cc27/phy_fsk.c)
target_link_libraries(cc27-radio_elf lrf_cc27xxx10)

# Radio bench: bench.c's shell (docs/bench.md) on UART0 at 921600 baud,
# radio.h, timer.h and uart.h implemented by cc27/radio.c, timer.c, uart.c
add_cc27_application(cc27-bench cc27/main.c bench.c frame.c sx1281.c cc27/radio.c cc27/timer.c
    cc27/uart.c cc27/rf.c cc27/lrf_hal.c cc27/phy_nesb.c cc27/phy_fsk250.c cc27/phy_fsk.c)
target_compile_definitions(cc27-bench_elf PRIVATE BENCH)
target_link_libraries(cc27-bench_elf lrf_cc27xxx10)

# The MPM port (cc27/main_mpm.c): the radio's link on UART0 (100000 8E2,
# cc27/mpm_port.c), the protocols of the MPM v2 build
add_cc27_application(cc27-mpm cc27/main_mpm.c cc27/mpm_port.c cc27/eeprom.c cc27/radio.c
    cc27/timer.c cc27/rf.c cc27/lrf_hal.c cc27/phy_nesb.c cc27/phy_fsk250.c cc27/phy_fsk.c
    frame.c log.c
    mpm/mpm.c mpm/mpm_frame.c mpm/mpm_settings.c mpm/protocols.c mpm/proto_test.c
    mpm/upstream/mpm_compat.c mpm/upstream/xn297.c mpm/upstream/Bayang_nrf24l01.c
    mpm/upstream/nrf24l01.c mpm/upstream/V2X2_nrf24l01.c mpm/upstream/XK_ccnrf.c
    mpm/upstream/cc2500.c mpm/upstream/frsky.c mpm/upstream/a7105.c mpm/upstream/FlySky_a7105.c
    mpm/upstream/AFHDS2A_a7105.c mpm/upstream/cyrf6936.c mpm/upstream/dsm_all.c
    mpm/upstream/telemetry.c)
target_link_libraries(cc27-mpm_elf lrf_cc27xxx10)
target_compile_definitions(cc27-mpm_elf PRIVATE
    MPM_PORT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR} MPM_PORT_VERSION_MINOR=${PROJECT_VERSION_MINOR})
if (FRSKY_CC26XX_PLAN)
    target_compile_definitions(cc27-mpm_elf PRIVATE FRSKY_CC26XX_PLAN)
endif()
# Another module's ID (e.g. 0x2bf3be8e, an MPM v2's): its bound receivers
set(CC27_MODULE_ID "" CACHE STRING "MPM module ID of the CC27 port (empty: the chip's)")
if (CC27_MODULE_ID)
    target_compile_definitions(cc27-mpm_elf PRIVATE MPM_MODULE_ID=${CC27_MODULE_ID})
endif()
set_source_files_properties(cc27/radio.c cc27/rf.c frame.c cc27/timer.c
    PROPERTIES COMPILE_OPTIONS $<$<CONFIG:Release>:-O2>
)
