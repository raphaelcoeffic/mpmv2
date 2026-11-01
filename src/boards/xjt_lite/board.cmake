# FrSky XJT-Lite (CC2650F128): the MPM port at 0x8000 behind FrSky's boot
# loader, kept as it is (it starts the image at 0x8000, no checks). The last
# sector (CCFG) is never written. Flashed with OpenOCD over cJTAG on a
# J-Link (a patched OpenOCD: github.com/raphaelcoeffic/openocd, jlink-cjtag)

message("## Target is FrSky XJT-Lite (CC2650F128): reduced MPM")

set(XJT_OPENOCD openocd CACHE STRING "OpenOCD with J-Link cJTAG support")

set(cc26xx_chip cc26x0)
include(platforms/cc26xx/platform.cmake)
# The reduced MPM: its protocols (mpm/multi/mpm_config.h, xjt_chip_sources)
list(APPEND board_definitions MPM_REDUCED)

# The MPM port: FrSky D and X, V2x2, XK (mpm/multi/mpm_config.h), without
# the A7105 and CYRF6936 emulations (RAM). firmware.elf and firmware.bin in
# the build directory; flash-firmware: at 0x8000, with the patched OpenOCD
set(xjt_chip_sources ${mpm_chip_sources})
list(REMOVE_ITEM xjt_chip_sources mpm/multi/a7105.c mpm/multi/cyrf6936.c)
add_board_app(firmware ${board_dir}/xjt_lite.ld
    core/main_mpm.c
    ${board_dir}/eeprom.c
    ${board_dir}/mpm_port.c
    ${cc26xx_hal_sources}
    core/frame.c
    core/log.c
    core/syscalls.c
    ${mpm_sources}
    ${xjt_chip_sources}
    ${platform_dir}/board.c
    ${board_dir}/board.c
)
target_mpm(firmware_elf)
add_custom_target(firmware ALL DEPENDS ${CMAKE_BINARY_DIR}/firmware.bin)

add_custom_target(flash-firmware
    COMMAND ${XJT_OPENOCD} -f board/frsky_xjt_lite.cfg
            -c "program ${CMAKE_BINARY_DIR}/firmware.bin 0x8000 verify reset exit"
    DEPENDS firmware
    USES_TERMINAL
    VERBATIM
)

# cmake --build <dir> --target flash: the MPM port
mpm_flash_method(flash_method debugger)
add_custom_target(flash)
add_dependencies(flash flash-firmware)
