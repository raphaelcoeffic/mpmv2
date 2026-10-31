# TI's LRF layer for the CC27xx radio (BSD-3-Clause), from the SimpleLink Low
# Power F3 SDK 4aa2c92 (source/ti/drivers/rcl, unchanged): image loading,
# register setup, trims, synthesizer and TX power, FIFOs. Not the RCL above
# it (scheduler, command handlers): src/platforms/cc27xx/rf.c drives the PBE
# instead. ti/log/Log.h, ti/drivers/dpl/HwiP.h and ti/drivers/Power.h are
# our stand-ins; the RCL_Hal functions LRF needs are in
# src/platforms/cc27xx/lrf_hal.c.

add_library(
    lrf_cc27xxx10
    OBJECT
    LRF.c
    lrf/LRFLPF3.c
    lrf/LRFLPF3_FIFO.c
)

target_link_libraries(lrf_cc27xxx10 PUBLIC driverlib_cc27xxx10)

target_include_directories(lrf_cc27xxx10 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

target_compile_definitions(lrf_cc27xxx10 PUBLIC DeviceFamily_CC27XXX10)
