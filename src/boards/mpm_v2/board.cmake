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
    ${cc26xx_dir}/board.c
    ${cc26xx_hal_sources}
    ${cc26xx_dir}/dma.c
    ${cc26xx_dir}/ppm.c
    ${cc26xx_dir}/spi.c
    ${board_dir}/board.c
    ${board_dir}/flash_driver.c
    ${board_dir}/board_app.c
    core/app_header.c
    ${board_dir}/mpm_port.c
    ${mpm_sources}
    core/eeprom.c
    core/file_server.c
    core/frame.c
    drivers/led_rgb.c
    core/lfs_driver.c
    core/log.c
    drivers/nor_flash.c
    core/settings.c
    core/syscalls.c
)

# Boot loader in sector 0 with CCFG, applications at 0x2000 (docs/update.md)
add_executable(bootloader ${board_dir}/bootloader/main.c ${cc26xx_dir}/board.c ${board_dir}/board.c)
target_compile_definitions(bootloader PRIVATE ${board_definitions})
target_compile_options(bootloader PRIVATE -Os)
target_include_directories(bootloader PRIVATE ${board_dir} ${cc26xx_dir} ${src_include_dirs})
target_link_libraries(bootloader ${cc26xx_driverlib})
target_link_options(bootloader PRIVATE -L${CMAKE_CURRENT_SOURCE_DIR}/linker)
set_target_properties(bootloader PROPERTIES
    LINKER_FILE ${board_dir}/bootloader.ld
    # Relinked when a script changes
    LINK_DEPENDS "${board_dir}/bootloader.ld;${CMAKE_CURRENT_SOURCE_DIR}/linker/sections.ld"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
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
    set(elf ${name}_elf)
    add_executable(${elf} ${firmware_sources} ${ARGN})
    target_include_directories(${elf} PRIVATE ${board_dir} ${cc26xx_dir} ${src_include_dirs})
    target_compile_definitions(${elf} PRIVATE ${board_definitions})
    target_mpm_definitions(${elf})
    if (DEBUG)
        target_compile_definitions(${elf} PRIVATE DEBUG)
    endif()
    target_link_libraries(${elf}
        ${cc26xx_driverlib}
        segger_rtt
        littlefs
    )
    target_link_options(${elf} PRIVATE -L${CMAKE_CURRENT_SOURCE_DIR}/linker)
    set_target_properties(${elf} PROPERTIES
        OUTPUT_NAME ${name}
        LINKER_FILE ${board_dir}/app.ld
        # Relinked when a script changes
        LINK_DEPENDS "${board_dir}/app.ld;${CMAKE_CURRENT_SOURCE_DIR}/linker/sections.ld"
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )

    # The whole flash: the application added to the boot loader at 0x2000
    set(app_section
        --add-section .app=${name}.bin
        --set-section-flags .app=alloc,load,readonly,contents
        --change-section-address .app=0x2000)
    set(images ${name}.bin ${file}-full.bin ${file}-full.hex)
    list(TRANSFORM images PREPEND ${CMAKE_BINARY_DIR}/)
    add_custom_command(
        OUTPUT ${images}
        COMMAND ${CMAKE_OBJCOPY} -O binary ${name}.elf ${name}.bin
        COMMAND ${CMAKE_OBJCOPY} -O binary --gap-fill 0xff ${app_section}
                bootloader.elf ${file}-full.bin
        COMMAND ${CMAKE_OBJCOPY} -O ihex ${app_section}
                bootloader.elf ${file}-full.hex
        DEPENDS ${elf} bootloader
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    if (Python3_Interpreter_FOUND)
        add_custom_command(
            OUTPUT ${CMAKE_BINARY_DIR}/${file}.bin
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/fw_image.py ${name}.bin
                    --version ${PROJECT_VERSION} --out ${file}.bin
            DEPENDS ${CMAKE_BINARY_DIR}/${name}.bin ${CMAKE_SOURCE_DIR}/tools/fw_image.py
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        )
        list(APPEND images ${CMAKE_BINARY_DIR}/${file}.bin)
    endif()
    add_custom_target(${name} ALL DEPENDS ${images})

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
add_application(bench mpm-v2-bench core/main_bench.c core/bench.c ${board_dir}/bench_port.c
    drivers/sx1281.c)
