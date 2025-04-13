
add_library(segger_rtt INTERFACE)

target_sources(segger_rtt
  INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/RTT/SEGGER_RTT_printf.c
  ${CMAKE_CURRENT_SOURCE_DIR}/RTT/SEGGER_RTT.c
)

target_include_directories(segger_rtt
    INTERFACE
    RTT
    Config
)
