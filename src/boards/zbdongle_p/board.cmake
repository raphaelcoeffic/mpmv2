# Sonoff ZBDongle-P (CC2652P): the radio bench as one image at 0x0 with the
# CCFG, no boot loader of ours: the chip's ROM boot loader over the dongle's
# USB (CP2102N, DTR/RTS on BOOT and reset) flashes it, as Sonoff's flasher
# does (tools/rom_flash.py --sonoff)

message("## Target is Sonoff ZBDongle-P (CC2652P): radio bench")

include(platforms/cc26xx/platform.cmake)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# bench.elf; zbdongle-p-bench.bin: the whole flash for the ROM boot loader
add_board_elf(bench_elf bench ${board_dir}/zbdongle_p.ld
    core/main_bench.c core/bench.c core/bench_fasst.c core/fasst_frame.c core/frame.c
    drivers/sx1281.c core/syscalls.c
    ${cc26xx_hal_sources} ${platform_dir}/dma.c ${platform_dir}/board.c
    ${board_dir}/board.c ${board_dir}/ccfg.c ${board_dir}/bench_port.c)
set(bench_image ${CMAKE_BINARY_DIR}/zbdongle-p-bench.bin)
add_custom_command(
    OUTPUT ${bench_image}
    COMMAND ${CMAKE_OBJCOPY} -O binary --gap-fill 0xff bench.elf ${bench_image}
    DEPENDS bench_elf
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
add_custom_target(bench ALL DEPENDS ${bench_image})

# flash-bench: through the ROM boot loader (port: --port from
# MULTI_FLASH_PORT, else the first CP2102N)
if (MULTI_FLASH_PORT)
    set(port --port ${MULTI_FLASH_PORT})
endif()
add_custom_target(flash-bench
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/rom_flash.py --sonoff ${bench_image} ${port}
    DEPENDS bench
    USES_TERMINAL
    VERBATIM
)
