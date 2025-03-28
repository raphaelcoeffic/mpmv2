
set(firmware_sources
    main.c
    syscalls.c
)

add_executable(firmware
    ${firmware_sources}
)

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
