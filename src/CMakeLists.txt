
set(firmware_sources
    board.c
    dma.c
    eeprom.c
    file_server.c
    flash_driver.c
    ihex.c
    lfs_driver.c
    led_rgb.c
    main.c
    mpm/mpm.c
    mpm/mpm_frame.c
    mpm/mpm_port.c
    mpm/mpm_settings.c
    mpm/protocols.c
    mpm/proto_test.c
    mpm/upstream/mpm_compat.c
    mpm/upstream/xn297.c
    mpm/upstream/Bayang_nrf24l01.c
    mpm/upstream/cc2500.c
    mpm/upstream/frsky.c
    mpm/upstream/a7105.c
    mpm/upstream/FlySky_a7105.c
    mpm/upstream/AFHDS2A_a7105.c
    mpm/upstream/cyrf6936.c
    mpm/upstream/dsm_all.c
    mpm/upstream/telemetry.c
    log.c
    nor_flash.c
    ppm.c
    frame.c
    radio.c
    serial.c
    settings.c
    shell.c
    spi.c
    syscalls.c
    timer.c
    uart.c
)

add_executable(firmware
    ${firmware_sources}
)
target_include_directories(firmware PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Release builds are -Os; the code on the radio's time-critical paths gets
# -O2: RF core commands and interrupts, the frame encoders and decoders
# (CYRF6936 sync search, DSM telemetry decoded between callbacks), the
# scheduler's alarms, and the bench's measurements (bench.c). -O3 measured
# the same (timing.py within 1 us, DSM telemetry 45.5 frames/s) for 9 KB more
set_source_files_properties(radio.c frame.c timer.c bench.c
    PROPERTIES COMPILE_OPTIONS $<$<CONFIG:Release>:-O2>
)

set(board_definitions "")
if (USE_XOSC)
    if (BOARD_CC2652R1_LAUNCHXL)
        message("## Target is CC2652R1-LAUNCHXL: use external oscillator")
        list(APPEND board_definitions BOARD_CC2652R1_LAUNCHXL)
    else()
        message("## Target is CC2652RB: use HPOSC (BAW)")
        list(APPEND board_definitions MCU_CC2652RB)
    endif()
    list(APPEND board_definitions USE_XOSC)
endif()
target_compile_definitions(firmware PRIVATE ${board_definitions}
    MPM_PORT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR} MPM_PORT_VERSION_MINOR=${PROJECT_VERSION_MINOR})

if (BOOTLOADER)
    # Boot loader in sector 0 with CCFG, application at 0x2000 (docs/update.md)
    message("## Boot loader: application at 0x2000")
    add_executable(bootloader bootloader/main.c board.c)
    target_compile_definitions(bootloader PRIVATE ${board_definitions})
    target_compile_options(bootloader PRIVATE -Os)
    target_include_directories(bootloader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(bootloader driverlib_cc13x2_cc26x2)
    target_link_options(bootloader PRIVATE -L${CMAKE_CURRENT_SOURCE_DIR}/linker)
    set_target_properties(bootloader PROPERTIES
        LINKER_FILE ${CMAKE_CURRENT_SOURCE_DIR}/linker/bootloader.ld
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    if (USE_CCFG)
        target_sources(bootloader PRIVATE ccfg.c)
    endif()
    add_custom_command(
        TARGET bootloader POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary bootloader.elf bootloader.bin
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
elseif (USE_CCFG)
    message("## Include CCFG")
    target_sources(firmware PRIVATE ccfg.c)
endif()

if (BENCH)
    message("## Radio bench mode")
    target_sources(firmware PRIVATE bench.c sx1281.c)
    target_compile_definitions(firmware PRIVATE BENCH)
endif()

if (DEBUG)
    message("## Debug output enabled (Segger RTT)")
    target_compile_definitions(firmware PRIVATE DEBUG)
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

target_link_libraries(firmware
    driverlib_cc13x2_cc26x2
    segger_rtt
    littlefs
    ihex
)

if (BOOTLOADER)
    target_link_options(firmware PRIVATE -L${CMAKE_CURRENT_SOURCE_DIR}/linker)
    set_target_properties(firmware
        PROPERTIES
        LINKER_FILE ${CMAKE_CURRENT_SOURCE_DIR}/linker/app.ld
    )
else()
    set_target_properties(firmware
        PROPERTIES
        LINKER_FILE ${TI_DEVICE_DIR}/cc13x2_cc26x2/linker_files/cc26x2r1f.lds
    )
endif()

# firmware.bin and firmware.elf directly in build directory
set_property(
    TARGET firmware
    PROPERTY RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_custom_command(
  TARGET firmware POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O binary ${OBJCOPY_ARGS} firmware.elf firmware.bin
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

if (BOOTLOADER)
  # firmware.bin gets the image header (length, CRC) the boot loader checks,
  # mpm-v2.bin is the file for the radio (MULTI signature appended),
  # mpm-v2-full.bin / .hex the whole flash for a first install (boot loader,
  # CCFG, application)
  add_custom_command(
    TARGET firmware POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary --gap-fill 0xff bootloader.elf bootloader.bin
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/fw_image.py firmware.bin
            --version ${PROJECT_VERSION} --out mpm-v2.bin
            --full bootloader.bin mpm-v2-full
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
  add_dependencies(firmware bootloader)
  # J-Link: ninja -C <build dir> flash (boot loader and application)
  add_custom_target(flash
    COMMAND ${CMAKE_SOURCE_DIR}/tools/flash_jlink.sh
            ${CMAKE_BINARY_DIR}/bootloader.elf ${CMAKE_BINARY_DIR}/firmware.bin@0x2000
    DEPENDS firmware bootloader
    USES_TERMINAL
  )
else()
  # J-Link: ninja -C <build dir> flash
  add_custom_target(flash
    COMMAND ${CMAKE_SOURCE_DIR}/tools/flash_jlink.sh ${CMAKE_BINARY_DIR}/firmware.elf
    DEPENDS firmware
    USES_TERMINAL
  )
endif()
