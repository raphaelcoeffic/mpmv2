# FrSky XJT-Lite (CC2650F128): applications at 0x8000 behind FrSky's boot
# loader, kept as it is (it starts the image at 0x8000, no checks). The last
# sector (CCFG) is never written. Flashed with OpenOCD over cJTAG on a
# J-Link (a patched OpenOCD: github.com/raphaelcoeffic/openocd, jlink-cjtag)

message("## Target is FrSky XJT-Lite (CC2650F128): reduced MPM")

set(XJT_OPENOCD openocd CACHE STRING "OpenOCD with J-Link cJTAG support")

set(cc26xx_chip cc26x0)
include(platforms/cc26xx/platform.cmake)
# The reduced MPM: its protocols (mpm/multi/mpm_config.h, xjt_chip_sources)
list(APPEND board_definitions MPM_REDUCED)

# Target <name>: <name>.elf and <name>.bin in the build directory, with
# board_init(). flash-<name>: the application only, at 0x8000
function(add_xjt_application name)
    add_board_app(${name} ${board_dir}/xjt_lite.ld ${ARGN} ${platform_dir}/board.c ${board_dir}/board.c)
    set(bin ${CMAKE_BINARY_DIR}/${name}.bin)
    add_custom_target(${name} ALL DEPENDS ${bin})

    add_custom_target(flash-${name}
        COMMAND ${XJT_OPENOCD} -f board/frsky_xjt_lite.cfg
                -c "program ${bin} 0x8000 verify reset exit"
        DEPENDS ${name}
        USES_TERMINAL
        VERBATIM
    )
endfunction()

# Bring-up: bay serial in, echoed on S.Port
add_xjt_application(xjtl-test ${board_dir}/xjtl_test.c ${platform_dir}/timer.c)

# Radio bring-up: a shell on the bay link driving the RF core
add_xjt_application(xjtl-radio ${board_dir}/xjtl_radio.c ${platform_dir}/timer.c
    ${cc26xx_radio_sources} core/syscalls.c)

# EEPROM pin probe
add_xjt_application(xjtl-eeprom ${board_dir}/xjtl_eeprom.c ${board_dir}/eeprom.c ${platform_dir}/timer.c
    core/log.c core/syscalls.c)

# The MPM port: FrSky D and X, V2x2, XK (mpm/multi/mpm_config.h), without
# the A7105 and CYRF6936 emulations (RAM)
set(xjt_chip_sources ${mpm_chip_sources})
list(REMOVE_ITEM xjt_chip_sources mpm/multi/a7105.c mpm/multi/cyrf6936.c)
add_xjt_application(firmware
    core/main_mpm.c
    ${board_dir}/eeprom.c
    ${board_dir}/mpm_port.c
    ${cc26xx_hal_sources}
    core/frame.c
    core/log.c
    core/syscalls.c
    ${mpm_sources}
    ${xjt_chip_sources}
)
target_mpm(firmware_elf)

# cmake --build <dir> --target flash: the MPM port, with the patched OpenOCD
mpm_flash_method(flash_method debugger)
add_custom_target(flash)
add_dependencies(flash flash-firmware)
