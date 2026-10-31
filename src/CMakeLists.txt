# The MULTI-Module core, the same on every board; mpm_port.c and the EEPROM
# are the board's (mpm/mpm_port.h, eeprom.h)
set(mpm_sources
    mpm/mpm.c
    mpm/mpm_frame.c
    mpm/mpm_settings.c
    mpm/protocols.c
    mpm/proto_test.c
    mpm/upstream/mpm_compat.c
    mpm/upstream/xn297.c
    mpm/upstream/Bayang_nrf24l01.c
    mpm/upstream/nrf24l01.c
    mpm/upstream/V2X2_nrf24l01.c
    mpm/upstream/XK_ccnrf.c
    mpm/upstream/cc2500.c
    mpm/upstream/frsky.c
    mpm/upstream/a7105.c
    mpm/upstream/FlySky_a7105.c
    mpm/upstream/AFHDS2A_a7105.c
    mpm/upstream/cyrf6936.c
    mpm/upstream/dsm_all.c
    mpm/upstream/telemetry.c
)

# What a target with the MULTI-Module core needs: its version (the module
# reports it to the radio), the FrSky frequency plan
function(target_mpm_definitions target)
    target_compile_definitions(${target} PRIVATE
        MPM_PORT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR} MPM_PORT_VERSION_MINOR=${PROJECT_VERSION_MINOR})
    if (FRSKY_CC26XX_PLAN)
        target_compile_definitions(${target} PRIVATE FRSKY_CC26XX_PLAN)
    endif()
endfunction()


set(firmware_sources
    board.c
    dma.c
    eeprom.c
    file_server.c
    flash_driver.c
    lfs_driver.c
    led_rgb.c
    main.c
    mpm/mpm_port.c
    ${mpm_sources}
    log.c
    nor_flash.c
    ppm.c
    frame.c
    radio.c
    serial.c
    settings.c
    spi.c
    syscalls.c
    timer.c
    uart.c
)

# Release builds are -Os; the code on the radio's time-critical paths gets
# -O2: RF core commands and interrupts, the frame encoders and decoders
# (CYRF6936 sync search, DSM telemetry decoded between callbacks), the
# scheduler's alarms, and the bench's measurements (bench.c). -O3 measured
# the same (timing.py within 1 us, DSM telemetry 45.5 frames/s) for 9 KB more
set_source_files_properties(radio.c frame.c timer.c bench.c
    PROPERTIES COMPILE_OPTIONS $<$<CONFIG:Release>:-O2>
)

set(board_definitions BOARD_${BOARD})
if (BOARD STREQUAL "CC2652R1_LAUNCHXL")
    message("## Target is CC2652R1-LAUNCHXL: 48 MHz xtal")
    # XDS110 on the LaunchPad
    set(flash_tool openocd -f board/ti_cc26x2_launchpad.cfg)
elseif (BOARD STREQUAL "MPM_V2")
    message("## Target is MPM v2 (CC2652RB): HPOSC (BAW)")
    list(APPEND board_definitions MCU_CC2652RB)
    # J-Link on J10
    set(flash_tool ${CMAKE_SOURCE_DIR}/tools/flash_jlink.sh)
elseif (BOARD STREQUAL "XJT_LITE")
    message("## Target is FrSky XJT-Lite (CC2650F128): reduced MPM")
elseif (BOARD STREQUAL "CC2755P10_LP")
    message("## Target is LP-EM-CC2755P10: radio bring-up")
endif()

if (DEBUG)
    message("## Debug output enabled (Segger RTT)")
else()
    message("## Debug output disabled")
    target_compile_definitions(littlefs
        INTERFACE
        LFS_NO_DEBUG
        LFS_NO_WARN
        LFS_NO_ERROR
        LFS_NO_ASSERT
    )
endif()

# XJT-Lite: CC26x0 driverlib, its own applications (xjt_lite.cmake)
if (BOARD STREQUAL "XJT_LITE")
    include(xjt_lite.cmake)
    return()
endif()

# CC2755P10 LaunchPad: the CC27xx platform, its own applications
# (boards/cc2755p10_lp/board.cmake)
if (BOARD STREQUAL "CC2755P10_LP")
    include(boards/cc2755p10_lp/board.cmake)
    return()
endif()

# Boot loader in sector 0 with CCFG, applications at 0x2000 (docs/update.md)
add_executable(bootloader bootloader/main.c board.c)
target_compile_definitions(bootloader PRIVATE ${board_definitions})
target_compile_options(bootloader PRIVATE -Os)
target_include_directories(bootloader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bootloader driverlib_cc13x2_cc26x2)
target_link_options(bootloader PRIVATE -L${CMAKE_CURRENT_SOURCE_DIR}/linker)
set_target_properties(bootloader PROPERTIES
    LINKER_FILE ${CMAKE_CURRENT_SOURCE_DIR}/linker/bootloader.ld
    # Relinked when a script changes
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/linker/bootloader.ld;${CMAKE_CURRENT_SOURCE_DIR}/linker/sections.ld"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
if (USE_CCFG)
    message("## Include CCFG")
    target_sources(bootloader PRIVATE ccfg.c)
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
    target_include_directories(${elf} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${elf} PRIVATE ${board_definitions})
    target_mpm_definitions(${elf})
    if (DEBUG)
        target_compile_definitions(${elf} PRIVATE DEBUG)
    endif()
    target_link_libraries(${elf}
        driverlib_cc13x2_cc26x2
        segger_rtt
        littlefs
    )
    target_link_options(${elf} PRIVATE -L${CMAKE_CURRENT_SOURCE_DIR}/linker)
    set_target_properties(${elf} PROPERTIES
        OUTPUT_NAME ${name}
        LINKER_FILE ${CMAKE_CURRENT_SOURCE_DIR}/linker/app.ld
        # Relinked when a script changes
        LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/linker/app.ld;${CMAKE_CURRENT_SOURCE_DIR}/linker/sections.ld"
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
add_application(firmware mpm-v2)

# Radio bench: test shell on UART0 instead of the MPM (docs/bench.md)
add_application(bench mpm-v2-bench bench.c sx1281.c)
target_compile_definitions(bench_elf PRIVATE BENCH)
