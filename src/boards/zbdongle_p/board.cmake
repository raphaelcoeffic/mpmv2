# Sonoff ZBDongle-P (CC2652P): the radio bench or the MPM port, each one
# image at 0x0 with the CCFG, no boot loader of ours: the chip's ROM boot
# loader over the dongle's USB (CP2102N, DTR/RTS on BOOT and reset) flashes
# it, as Sonoff's flasher does (tools/rom_flash.py --sonoff)

message("## Target is Sonoff ZBDongle-P (CC2652P): radio bench, MPM port")

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

# The MPM port: the radio's link on UART0 through the CP2102N (100000 8E2,
# mpm_port.c; tools/bench/radio_sim.py), the protocols of the MPM v2 build,
# the EEPROM in RAM (eeprom_ram.c) and the host's file "eeprom"
# (file_server_ram.c). firmware.elf; zbdongle-p-firmware.bin: the whole
# flash; flash-firmware: through the ROM boot loader, as flash-bench
add_board_elf(firmware_elf firmware ${board_dir}/zbdongle_p.ld
    core/main_mpm.c ${board_dir}/mpm_port.c core/eeprom_ram.c core/file_server_ram.c
    core/fasst_frame.c core/frame.c core/log.c core/syscalls.c ${mpm_sources} ${mpm_chip_sources}
    ${cc26xx_hal_sources} ${platform_dir}/dma.c ${platform_dir}/board.c
    ${board_dir}/board.c ${board_dir}/ccfg.c)
target_mpm(firmware_elf)
set(firmware_image ${CMAKE_BINARY_DIR}/zbdongle-p-firmware.bin)
add_custom_command(
    OUTPUT ${firmware_image}
    COMMAND ${CMAKE_OBJCOPY} -O binary --gap-fill 0xff firmware.elf ${firmware_image}
    DEPENDS firmware_elf
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
add_custom_target(firmware ALL DEPENDS ${firmware_image})
add_custom_target(flash-firmware
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/rom_flash.py --sonoff ${firmware_image} ${port}
    DEPENDS firmware
    USES_TERMINAL
    VERBATIM
)

# cmake --build <dir> --target flash: the MPM port
mpm_flash_method(flash_method usb-rom)
add_custom_target(flash)
add_dependencies(flash flash-firmware)

# sniffer-web: the static FASST sniffer page (tools/sniffer_web, mpm-v2's
# only) with the image, ready to publish as is (e.g. Cloudflare Pages:
# sniffer-web/)
if (EXISTS ${CMAKE_SOURCE_DIR}/tools/sniffer_web/index.html)
    set(web_dir ${CMAKE_BINARY_DIR}/sniffer-web)
    add_custom_target(sniffer-web
        COMMAND ${CMAKE_COMMAND} -E make_directory ${web_dir}
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/tools/sniffer_web/index.html ${web_dir}/
        COMMAND ${CMAKE_COMMAND} -E copy ${bench_image} ${web_dir}/fasst-sniffer-zbdongle-p.bin
        DEPENDS bench
        VERBATIM
    )
endif()
