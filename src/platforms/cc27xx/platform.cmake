# CC27xx: radio.h, timer.h and uart.h on the LRF radio (TI's LRF layer,
# lib/ti/drivers/rcl, without the RCL), the system timer and UART0 (polled,
# board_uart_init()). board.c (cc27xx.h), in every application: the chip's
# power-up, HFXT, microseconds and UART0, the board's pins after. The PHY
# settings come from tools/cc27_phy.py; phy_ble.c for the radio bring-up only

set(cc27xx_dir ${CMAKE_CURRENT_LIST_DIR})

set(cc27xx_hal_sources radio.c timer.c uart.c)
list(TRANSFORM cc27xx_hal_sources PREPEND ${cc27xx_dir}/)

set(cc27xx_rf_sources rf.c lrf_hal.c phy_nesb.c phy_fsk250.c phy_fsk.c)
list(TRANSFORM cc27xx_rf_sources PREPEND ${cc27xx_dir}/)

# Radio timing (src/CMakeLists.txt)
set_source_files_properties(${cc27xx_dir}/radio.c ${cc27xx_dir}/rf.c ${cc27xx_dir}/timer.c
    PROPERTIES COMPILE_OPTIONS $<$<CONFIG:Release>:-O2>
)
