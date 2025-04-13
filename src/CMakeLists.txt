
set(firmware_sources
    board.c
    dma.c
    led_rgb.c
    main.c
    spi.c
    syscalls.c
    timer.c
    uart.c
)

add_executable(firmware
    ${firmware_sources}
)

if (USE_XOSC)
    if (BOARD_CC2652R1_LAUNCHXL)
        message("## Target is CC2652R1-LAUNCHXL: use external oscillator")
        target_compile_definitions(firmware PRIVATE BOARD_CC2652R1_LAUNCHXL)
    else()
        message("## Target is CC2652RB: use HPOSC (BAW)")
        target_compile_definitions(firmware PRIVATE MCU_CC2652RB)
    endif()
    target_compile_definitions(firmware PRIVATE USE_XOSC)
endif()

# Uncomment to use compiled code instead of ROM
#
# target_compile_definitions(
#     driverlib_cc13x2_cc26x2 PUBLIC
#     DRIVERLIB_NOROM
# )

if (USE_CCFG)
    message("## Include CCFG")
    target_sources(firmware PRIVATE ccfg.c)
endif()

if (DEBUG)
    message("## Debug output enabled (Segger RTT)")
    target_compile_definitions(firmware PRIVATE DEBUG)
endif()

target_link_libraries(firmware
    driverlib_cc13x2_cc26x2
    segger_rtt
)

set_target_properties(firmware
    PROPERTIES
    LINKER_FILE ${TI_DEVICE_DIR}/cc13x2_cc26x2/linker_files/cc26x2r1f.lds
)

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
