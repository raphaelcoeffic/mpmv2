# CC26xx, the CC2652 and the CC2650: radio.h on the RF core (radio.c, the
# chip's part in rf_<chip>.c), timer.h, uart.h, and board_init() (board.c,
# the board's board_pins_init() after it). dma.c, spi.c (spi.h) and ppm.c
# (ppm.h) for the boards that use them.
#
# The board sets cc26xx_chip before: cc26x2 (default) or cc26x0 (MCU_CC26X0:
# TI's generic FSK patches, one UART, no DMA, the CC26x0 driverlib). Gives
# cc26xx_driverlib, the driverlib to link

set(cc26xx_dir ${CMAKE_CURRENT_LIST_DIR})

if (NOT cc26xx_chip)
    set(cc26xx_chip cc26x2)
endif()
if (cc26xx_chip STREQUAL "cc26x0")
    list(APPEND board_definitions MCU_CC26X0)
    set(cc26xx_driverlib driverlib_cc26x0)
elseif (cc26xx_chip STREQUAL "cc26x2")
    set(cc26xx_driverlib driverlib_cc13x2_cc26x2)
else()
    message(FATAL_ERROR "cc26xx_chip=${cc26xx_chip}: cc26x2 or cc26x0")
endif()

# radio.h alone, and with timer.h and uart.h
set(cc26xx_radio_sources radio.c rf_${cc26xx_chip}.c)
list(TRANSFORM cc26xx_radio_sources PREPEND ${cc26xx_dir}/)
set(cc26xx_hal_sources ${cc26xx_radio_sources} ${cc26xx_dir}/timer.c ${cc26xx_dir}/uart.c)

# Radio timing (src/CMakeLists.txt)
set_source_files_properties(${cc26xx_radio_sources} ${cc26xx_dir}/timer.c
    PROPERTIES COMPILE_OPTIONS $<$<CONFIG:Release>:-O2>
)
