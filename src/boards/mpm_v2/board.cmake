# MPM v2 (CC2652RB) and the CC2652R1 LaunchPad standing in for it (its
# board.h): the boot loader, the MPM port and the radio bench

include(platforms/cc26xx/platform.cmake)

if (BOARD STREQUAL "CC2652R1_LAUNCHXL")
    message("## Target is CC2652R1-LAUNCHXL: 48 MHz xtal")
    # XDS110 on the LaunchPad
    set(flash_tool openocd -f board/ti_cc26x2_launchpad.cfg)
else()
    message("## Target is MPM v2 (CC2652RB): HPOSC (BAW)")
    list(APPEND board_definitions MCU_CC2652RB)
    # J-Link on J10
    set(flash_tool ${CMAKE_SOURCE_DIR}/tools/flash_jlink.sh)
endif()

set(firmware_sources
    ${platform_dir}/board.c
    ${cc26xx_hal_sources}
    ${platform_dir}/dma.c
    ${platform_dir}/ppm.c
    ${platform_dir}/spi.c
    ${board_dir}/board.c
    ${board_dir}/flash_driver.c
    ${board_dir}/board_app.c
    core/app_header.c
    ${board_dir}/mpm_port.c
    ${mpm_sources}
    ${mpm_chip_sources}
    core/eeprom.c
    core/file_server.c
    core/fasst_frame.c
    core/frame.c
    drivers/led_rgb.c
    core/lfs_driver.c
    core/log.c
    drivers/nor_flash.c
    core/settings.c
    core/syscalls.c
)

# Boot loader in sector 0 with CCFG, applications at 0x2000 (docs/update.md)
add_board_elf(bootloader bootloader ${board_dir}/bootloader.ld
    core/bootloader.c ${platform_dir}/bl_port.c ${platform_dir}/board.c ${board_dir}/board.c)
target_compile_options(bootloader PRIVATE -Os)
if (USE_CCFG)
    message("## Include CCFG")
    target_sources(bootloader PRIVATE ${board_dir}/ccfg.c)
endif()
add_custom_command(
    TARGET bootloader POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary --gap-fill 0xff bootloader.elf bootloader.bin
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# The file for the radio needs the image CRC: tools/fw_image.py
find_package(Python3 COMPONENTS Interpreter)
if (NOT Python3_Interpreter_FOUND)
    message("## No Python 3: no files for the radio (mpm-v2.bin)")
endif()

# An application behind the boot loader. Target <name>: in the build
# directory, <name>.elf, <name>.bin with the image header from app.ld (no
# CRC, already checked), <file>-full.bin / .hex the whole flash for a first
# install (boot loader, CCFG, application), made again when the boot loader
# changes, and with Python <file>.bin for the radio (CRC, MULTI signature).
# flash-<name>: boot loader and application through the board's debug probe.
function(add_application name file)
    add_board_app(${name} ${board_dir}/app.ld ${firmware_sources} ${ARGN})
    target_mpm(${name}_elf)
    if (DEBUG)
        target_compile_definitions(${name}_elf PRIVATE DEBUG)
    endif()
    target_link_libraries(${name}_elf segger_rtt littlefs)

    # The whole flash: the application added to the boot loader at 0x2000
    set(app_section
        --add-section .app=${name}.bin
        --set-section-flags .app=alloc,load,readonly,contents
        --change-section-address .app=0x2000)
    set(images ${file}-full.bin ${file}-full.hex)
    list(TRANSFORM images PREPEND ${CMAKE_BINARY_DIR}/)
    add_custom_command(
        OUTPUT ${images}
        COMMAND ${CMAKE_OBJCOPY} -O binary --gap-fill 0xff ${app_section}
                bootloader.elf ${file}-full.bin
        COMMAND ${CMAKE_OBJCOPY} -O ihex ${app_section}
                bootloader.elf ${file}-full.hex
        DEPENDS ${CMAKE_BINARY_DIR}/${name}.bin bootloader
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    if (Python3_Interpreter_FOUND)
        add_update_image(${name} ${file}.bin)
        list(APPEND images ${CMAKE_BINARY_DIR}/${file}.bin)
    endif()
    add_custom_target(${name} ALL DEPENDS ${CMAKE_BINARY_DIR}/${name}.bin ${images})

    if (BOARD STREQUAL "CC2652R1_LAUNCHXL")
        set(flash_args
            -c "program ${CMAKE_BINARY_DIR}/bootloader.elf verify"
            -c "program ${CMAKE_BINARY_DIR}/${name}.bin 0x2000 verify reset exit")
    else()
        set(flash_args
            ${CMAKE_BINARY_DIR}/bootloader.elf ${CMAKE_BINARY_DIR}/${name}.bin@0x2000)
    endif()
    add_custom_target(flash-${name}
        COMMAND ${flash_tool} ${flash_args}
        DEPENDS ${name}
        USES_TERMINAL
        VERBATIM
    )
endfunction()

# The MPM port (the module EdgeTX drives)
add_application(firmware mpm-v2 core/main_mpm.c)

# Radio bench: test shell on UART0 instead of the MPM (docs/bench.md)
add_application(bench mpm-v2-bench core/main_bench.c core/bench.c core/bench_fasst.c ${board_dir}/bench_port.c
    core/tfhss.c drivers/sx1281.c)

# cmake --build <dir> --target flash: the MPM firmware.
#   usb       MPM v2: through its USB-C port and the MPM v2 boot loader, as
#             the radio flashes it (tools/stk500_flash.py, mpm-v2.bin)
#   usb-rom   MPM v2: through its USB-C port and the chip's ROM boot loader,
#             boot loader and firmware (tools/rom_flash.py, mpm-v2-full.bin):
#             the first install, or a module that no longer starts
#   radio     MPM v2 in the radio's bay, through the radio's USB (EdgeTX
#             with the external module pass-through, its CLI on
#             MULTI_FLASH_PORT)
#   debugger  flash-firmware: MPM v2 with a J-Link, LaunchPad with its XDS110
if (BOARD STREQUAL "MPM_V2")
    mpm_flash_method(flash_method usb usb-rom radio debugger)
else()
    mpm_flash_method(flash_method debugger)
endif()
if (flash_method STREQUAL "debugger")
    add_custom_target(flash)
    add_dependencies(flash flash-firmware)
elseif (Python3_Interpreter_FOUND)
    if (MULTI_FLASH_PORT)
        set(port --port ${MULTI_FLASH_PORT})
    endif()
    if (flash_method STREQUAL "usb-rom")
        set(flash_cmd ${CMAKE_SOURCE_DIR}/tools/rom_flash.py ${CMAKE_BINARY_DIR}/mpm-v2-full.bin ${port})
    elseif (flash_method STREQUAL "radio")
        set(flash_cmd ${CMAKE_SOURCE_DIR}/tools/stk500_flash.py ${CMAKE_BINARY_DIR}/mpm-v2.bin ${port} --radio)
    else()
        set(flash_cmd ${CMAKE_SOURCE_DIR}/tools/stk500_flash.py ${CMAKE_BINARY_DIR}/mpm-v2.bin ${port})
    endif()
    add_custom_target(flash
        COMMAND ${Python3_EXECUTABLE} ${flash_cmd}
        DEPENDS firmware
        USES_TERMINAL
        VERBATIM
    )
endif()
