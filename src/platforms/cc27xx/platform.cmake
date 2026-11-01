# CC27xx: radio.h, timer.h and uart.h on the LRF radio (TI's LRF layer,
# lib/ti/drivers/rcl, without the RCL), the system timer and UART0
# (board_uart_init()). board.c (cc27xx.h), in every application: the chip's
# power-up, HFXT, microseconds and UART0, the board's pins after. The PHY
# settings come from tools/cc27_phy.py. Gives platform_dir and
# platform_driverlib (add_board_elf())

set(platform_dir ${CMAKE_CURRENT_LIST_DIR})
set(platform_driverlib driverlib_cc27xxx10)

set(cc27xx_hal_sources radio.c timer.c uart.c)
list(TRANSFORM cc27xx_hal_sources PREPEND ${platform_dir}/)

set(cc27xx_rf_sources rf.c lrf_hal.c phy_nesb.c phy_fsk250.c phy_fsk.c)
list(TRANSFORM cc27xx_rf_sources PREPEND ${platform_dir}/)

# Radio timing (src/CMakeLists.txt)
set_source_files_properties(${platform_dir}/radio.c ${platform_dir}/rf.c ${platform_dir}/timer.c
    PROPERTIES COMPILE_OPTIONS $<$<CONFIG:Release>:-O2>
)
