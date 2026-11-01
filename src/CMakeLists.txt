# The code shared by the boards: core (no MCU code: frames, log, storage,
# the radio bench), hal (the interfaces a platform or board implements:
# radio.h, timer.h, uart.h...), drivers (chips on hal's spi.h) and mpm (the
# MULTI-Module core). platforms/<family> implements hal for an MCU family,
# boards/<name> holds a board's pins, applications and flashing
set(src_include_dirs
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/hal
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers
    # "mpm/..."
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# The MULTI-Module core, the same on every board; mpm_port.c and the EEPROM
# are the board's (mpm/mpm_port.h, eeprom.h). mpm/multi: what the protocol
# files of the MULTI-Module expect around them (Multiprotocol.ino's globals,
# telemetry, mpm_config.h for _Config.h) and the RF chips they drive,
# emulated on the radio (mpm_chip_sources)
set(mpm_dir ${CMAKE_CURRENT_SOURCE_DIR}/mpm)
set(mpm_sources
    mpm/mpm.c
    mpm/mpm_frame.c
    mpm/mpm_settings.c
    mpm/protocols.c
    mpm/proto_test.c
    mpm/multi/mpm_compat.c
    mpm/multi/telemetry.c
)
set(mpm_chip_sources
    mpm/multi/nrf24l01.c
    mpm/multi/xn297.c
    mpm/multi/cc2500.c
    mpm/multi/a7105.c
    mpm/multi/cyrf6936.c
)

# The protocol files, target_mpm_protocols(<target>): in the MULTI-Module
# repo Multiprotocol/*.ino themselves (mpm/sketch), in mpm-v2 their C copies
# (mpm/upstream). The rest of src/ is the same in both
if (EXISTS ${mpm_dir}/sketch)
    include(mpm/sketch/protocols.cmake)
else()
    include(mpm/upstream/protocols.cmake)
endif()

# A target with the MULTI-Module core (mpm_sources, mpm_chip_sources): the
# protocol files, the version (the module reports it to the radio), the
# FrSky frequency plan
function(target_mpm target)
    target_include_directories(${target} PRIVATE ${mpm_dir}/multi)
    target_mpm_protocols(${target})
    target_compile_definitions(${target} PRIVATE
        MPM_PORT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR} MPM_PORT_VERSION_MINOR=${PROJECT_VERSION_MINOR})
    if (FRSKY_CC26XX_PLAN)
        target_compile_definitions(${target} PRIVATE FRSKY_CC26XX_PLAN)
    endif()
endfunction()

# cmake --build <dir> --target flash: the board's MPM firmware, by
# MULTI_FLASH_METHOD (the board's methods, board.cmake; empty: its first).
# mpm_flash_method(<var> <methods>...): the method, checked
set(MULTI_FLASH_METHOD "" CACHE STRING
  "Flash method: usb, usb-rom, radio, debugger (empty: the board's first)")
set(MULTI_FLASH_PORT "" CACHE STRING
  "Serial port: the MPM v2's USB-C (empty: the first CH343, or V2_PORT in the environment), the radio's for radio")
function(mpm_flash_method var)
    if (MULTI_FLASH_METHOD)
        set(method ${MULTI_FLASH_METHOD})
    else()
        list(GET ARGN 0 method)
    endif()
    if (NOT method IN_LIST ARGN)
        message(FATAL_ERROR "MULTI_FLASH_METHOD=${method}: one of ${ARGN} for ${BOARD}")
    endif()
    set(${var} ${method} PARENT_SCOPE)
endfunction()

# add_board_elf(<target> <name> <ld> <sources>...): <name>.elf in the build
# directory, linked by <ld> (with linker/sections.ld) against the
# platform's driverlib (platform_driverlib, platform.cmake), with the
# board's and the platform's headers
function(add_board_elf target name ld)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE ${board_dir} ${platform_dir} ${src_include_dirs})
    target_compile_definitions(${target} PRIVATE ${board_definitions})
    target_link_libraries(${target} ${platform_driverlib})
    target_link_options(${target} PRIVATE -L${CMAKE_CURRENT_FUNCTION_LIST_DIR}/linker)
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME ${name}
        LINKER_FILE ${ld}
        # Relinked when a script changes
        LINK_DEPENDS "${ld};${CMAKE_CURRENT_FUNCTION_LIST_DIR}/linker/sections.ld"
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endfunction()

# add_board_app(<name> <ld> <sources>...): add_board_elf() as <name>_elf,
# and <name>.bin in the build directory (the board's <name> target builds it)
function(add_board_app name ld)
    add_board_elf(${name}_elf ${name} ${ld} ${ARGN})
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/${name}.bin
        COMMAND ${CMAKE_OBJCOPY} -O binary ${name}.elf ${name}.bin
        DEPENDS ${name}_elf
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endfunction()

# add_update_image(<name> <file>): <file> in the build directory, <name>.bin
# for an update through the boot loader, from the radio or STK500 (CRC,
# MULTI signature: tools/fw_image.py)
function(add_update_image name file)
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/${file}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/fw_image.py ${name}.bin
                --version ${PROJECT_VERSION} --out ${file}
        DEPENDS ${CMAKE_BINARY_DIR}/${name}.bin ${CMAKE_SOURCE_DIR}/tools/fw_image.py
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endfunction()

# Release builds are -Os; the code on the radio's time-critical paths gets
# -O2: RF core commands and interrupts, the frame encoders and decoders
# (CYRF6936 sync search, DSM telemetry decoded between callbacks), the
# scheduler's alarms, and the bench's measurements (bench.c). -O3 measured
# the same (timing.py within 1 us, DSM telemetry 45.5 frames/s) for 9 KB more.
# The platforms' files: their platform.cmake
set_source_files_properties(core/frame.c core/bench.c
    PROPERTIES COMPILE_OPTIONS $<$<CONFIG:Release>:-O2>
)

set(board_definitions BOARD_${BOARD})

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

# The board: its board.h, applications and flashing (boards/<name>/board.cmake),
# with its MCU family's code (platforms/<family>/platform.cmake)
if (BOARD STREQUAL "MPM_V2" OR BOARD STREQUAL "CC2652R1_LAUNCHXL")
    set(board_dir ${CMAKE_CURRENT_SOURCE_DIR}/boards/mpm_v2)
elseif (BOARD STREQUAL "XJT_LITE")
    set(board_dir ${CMAKE_CURRENT_SOURCE_DIR}/boards/xjt_lite)
elseif (BOARD STREQUAL "CC2755P10_LP")
    set(board_dir ${CMAKE_CURRENT_SOURCE_DIR}/boards/cc2755p10_lp)
endif()
include(${board_dir}/board.cmake)
