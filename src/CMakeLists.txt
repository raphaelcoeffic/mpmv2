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

# Release builds are -Os; the code on the radio's time-critical paths gets
# -O2: RF core commands and interrupts, the frame encoders and decoders
# (CYRF6936 sync search, DSM telemetry decoded between callbacks), the
# scheduler's alarms, and the bench's measurements (bench.c). -O3 measured
# the same (timing.py within 1 us, DSM telemetry 45.5 frames/s) for 9 KB more.
# The platforms' files: their platform.cmake
set_source_files_properties(frame.c bench.c
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
