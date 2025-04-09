
set(firmware_sources
    main.c
    timer.c
    uart.c
    syscalls.c
)

add_executable(firmware
    ${firmware_sources}
)

# Comment to use internal oscillator
# 
# Please note: USE_XOSC is required to use the CC2652RB's HPOSC
#
target_compile_definitions(firmware PRIVATE USE_XOSC)

# Uncomment to use compiled code instead of ROM
#
# target_compile_definitions(
#     driverlib_cc13x2_cc26x2 PUBLIC
#     DRIVERLIB_NOROM
# )

# Comment to remove CCFG from firmware
#
target_sources(firmware PRIVATE ccfg.c)

# Comment if the target MCU is NOT CC2652RB
#
target_compile_definitions(firmware PRIVATE CC2652RB)


target_link_libraries(firmware
    driverlib_cc13x2_cc26x2
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
