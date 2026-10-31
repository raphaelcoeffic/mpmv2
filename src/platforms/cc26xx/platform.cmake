# CC26xx, the CC2652 and the CC2650 (MCU_CC26X0: TI's generic FSK patches,
# one UART, no DMA): radio.h on the RF core, timer.h, uart.h, and
# board_init() (board.c, the board's board_pins_init() after it). dma.c,
# spi.c (spi.h) and ppm.c (ppm.h) for the boards that use them

set(cc26xx_dir ${CMAKE_CURRENT_LIST_DIR})

set(cc26xx_hal_sources radio.c timer.c uart.c)
list(TRANSFORM cc26xx_hal_sources PREPEND ${cc26xx_dir}/)

# Radio timing (src/CMakeLists.txt)
set_source_files_properties(${cc26xx_dir}/radio.c ${cc26xx_dir}/timer.c
    PROPERTIES COMPILE_OPTIONS $<$<CONFIG:Release>:-O2>
)
