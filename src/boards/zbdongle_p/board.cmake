# Sonoff ZBDongle-P (CC2652P): the radio bench or the MPM port, each one
# image at 0x0 with the CCFG, no boot loader of ours: the chip's ROM boot
# loader over the dongle's USB (CP2102N, DTR/RTS on BOOT and reset) flashes
# it, as Sonoff's flasher does (tools/rom_flash.py --sonoff)

message("## Target is Sonoff ZBDongle-P (CC2652P): radio bench, MPM port")

include(platforms/cc26xx/platform.cmake)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# flash-<name>: through the ROM boot loader (port: --port from
# MULTI_FLASH_PORT, else the first CP2102N)
if (MULTI_FLASH_PORT)
    set(port --port ${MULTI_FLASH_PORT})
endif()

# Target <name>: in the build directory <name>.elf and
# zbdongle-p-<name>.bin, the whole flash (CCFG included) for the ROM boot
# loader. flash-<name>: that image, as Sonoff's flasher does
function(add_application name)
    add_board_elf(${name}_elf ${name} ${board_dir}/zbdongle_p.ld ${ARGN}
        ${cc26xx_hal_sources} ${platform_dir}/dma.c ${platform_dir}/board.c
        ${board_dir}/board.c ${board_dir}/ccfg.c)
    set(image ${CMAKE_BINARY_DIR}/zbdongle-p-${name}.bin)
    add_custom_command(
        OUTPUT ${image}
        COMMAND ${CMAKE_OBJCOPY} -O binary --gap-fill 0xff ${name}.elf ${image}
        DEPENDS ${name}_elf
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    add_custom_target(${name} ALL DEPENDS ${image})
    add_custom_target(flash-${name}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/rom_flash.py --sonoff ${image} ${port}
        DEPENDS ${name}
        USES_TERMINAL
        VERBATIM
    )
endfunction()

# Radio bench (docs/bench.md)
add_application(bench core/main_bench.c core/bench.c core/bench_fasst.c core/fasst_frame.c
    core/frame.c drivers/sx1281.c core/syscalls.c ${board_dir}/bench_port.c)

# The MPM port: the radio's link on UART0 through the CP2102N (100000 8E2,
# mpm_port.c; tools/bench/radio_sim.py), the protocols of the MPM v2 build,
# the EEPROM in RAM (eeprom_ram.c) and the host's file "eeprom"
# (file_server_ram.c)
add_application(firmware core/main_mpm.c ${board_dir}/mpm_port.c core/eeprom_ram.c
    core/file_server_ram.c core/fasst_frame.c core/frame.c core/log.c core/syscalls.c
    ${mpm_sources} ${mpm_chip_sources})
target_mpm(firmware_elf)

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
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/zbdongle-p-bench.bin
                ${web_dir}/fasst-sniffer-zbdongle-p.bin
        DEPENDS bench
        VERBATIM
    )
endif()
