# The protocol files of mpm-v2: C copies of Multiprotocol/*.ino (changes
# marked mpm-v2), with copies of their iface_*.h, Convert.ino (convert.c)
# and chips.h for the emulations. Each is under its *_INO define, as
# upstream (mpm/multi/mpm_config.h). The MULTI-Module repo builds
# Multiprotocol/*.ino instead (mpm/sketch/protocols.cmake)

set(mpm_protocol_sources
    convert.c
    Bayang_nrf24l01.c
    V2X2_nrf24l01.c
    XK_ccnrf.c
    frsky.c
    FlySky_a7105.c
    AFHDS2A_a7105.c
    dsm_all.c
)
list(TRANSFORM mpm_protocol_sources PREPEND ${CMAKE_CURRENT_LIST_DIR}/)

function(target_mpm_protocols target)
    target_sources(${target} PRIVATE ${mpm_protocol_sources})
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
endfunction()
