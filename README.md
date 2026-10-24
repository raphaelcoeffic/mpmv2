# mpm-v2

Firmware for a MULTI-Module built around a single 2.4 GHz SoC (CC2652 for
now) instead of the four radio chips of the 4-in-1.

Radio work so far, see `docs/`:

- [docs/status.md](docs/status.md): what has been done, where things are, next steps toward MPM v2
- [docs/cc2652-radio-assessment.md](docs/cc2652-radio-assessment.md): what the CC2652 radio can and cannot do for the MPM protocols, requirements for any other chip
- [docs/interop-results.md](docs/interop-results.md): over-the-air results against the CC2500, NRF24L01, A7105 and CYRF6936 of a 4-in-1 module
- [docs/hardware.md](docs/hardware.md): the MPM v2 board: pins, RF chain, radio bay, changes for the next revision
- [docs/bench.md](docs/bench.md): the test rig (bench firmwares, host tools in `tools/bench`, unit tests in `tests/frame`)
- [docs/rf-core-notes.md](docs/rf-core-notes.md): what was found inside the CC2652 RF core (modem registers, CPE parameters)
- [docs/radio-interface.md](docs/radio-interface.md): the radio driver interface, what the MPM protocols need from it, the CC2652's measured latencies
- [docs/mpm-core.md](docs/mpm-core.md): the MULTI-Module core (`src/mpm`): serial link with the radio, scheduler, telemetry, test protocol

Credits: the RF core work builds on the 38C3 talk "Beyond BLE: Cracking open
the black-box of RF microcontrollers" by Adam Batori and Robert Pafford
([talk](https://events.ccc.de/congress/2024/hub/en/event/beyond-ble-cracking-open-the-black-box-of-rf-microcontrollers/),
[tools](https://github.com/rjp5th/beyond-ble-tools)): thanks for the ideas
and the inspiration.

## Trying it

What it takes: the MPM v2 board, a USB-C cable, and Python 3.10 or later
with [uv](https://docs.astral.sh/uv/) (the tools declare their packages,
`uv run` installs them; without uv: `pip install pyserial` and
`python3 tools/<script>`). No debugger needed. Linux and macOS see the
board's CH343 as a standard USB serial port; on Windows, if no COM port
appears, install WCH's CH343 driver.

The firmware files (from a release, or [built](#building) in `build/`):

| File | What | For |
|---|---|---|
| `mpm-v2-full.bin`, `mpm-v2-full.hex` | the whole internal flash: boot loader, application, CCFG | the first install, or a recovery |
| `mpm-v2.bin` | the application with the MULTI signature | updates, through the MPM v2 boot loader |

### First install, over USB-C

```bash
uv run tools/rom_flash.py mpm-v2-full.bin
```

`rom_flash.py` opens the CC2652's ROM boot loader through the CH343 (a blank
chip opens it anyway), erases the internal flash, writes the image and
checks it by CRC, then resets: 5 to 15 s. It works whatever the chip holds,
so it is also the way back from a bad update. The external flash (EEPROM
file, settings) is not touched. `--info` reads the chip and writes nothing;
the port is the first CH343, or `--port` / `$V2_PORT`.

With a debugger on J10 instead (J-Link, XDS110...): write
`mpm-v2-full.hex` with any tool that takes one, e.g. OpenOCD
(`tools/flash_jlink.sh` shows the commands). TI's UniFlash should take it
too, through a debugger or the ROM boot loader (not tried).

### Is it running?

```bash
uv run tools/mpm_fs.py log -t 2      # the module's debug log since power up
uv run tools/mpm_fs.py info          # its file system (the external flash)
```

The log starts with `## Boot completed ##`, the file system and the
settings read; then the radio's input and each protocol start show up as
they come. What the module does, its settings files (`ppm.txt`,
`debug.txt`) and the log: [docs/mpm-core.md](docs/mpm-core.md).

### Updates

- **Over USB-C:** `uv run tools/stk500_flash.py mpm-v2.bin --verify`
  (about 30 s; through the MPM v2 boot loader, as a radio does it), or
  `rom_flash.py mpm-v2-full.bin` again (faster, the same result).
- **From the radio** ("Flash External Multi" with `mpm-v2.bin`): needs an
  EdgeTX build with MPM v2 support, not in an EdgeTX release yet: branch
  [`mpm-v2-passthrough-2.12`](https://github.com/EdgeTX/edgetx/tree/mpm-v2-passthrough-2.12)
  (EdgeTX 2.12 with the MPM v2 files and the pass-through below). Tested on
  a TX15: see [docs/update.md](docs/update.md#tested-on-a-radio).
- **Through the radio's USB** (the same EdgeTX branch, CLI on the radio's
  USB serial): `uv run tools/stk500_flash.py mpm-v2.bin --port <radio port>
  --radio --verify`, the radio passing the bytes to the external bay.

## Building

Prerequisites: the [Arm GNU
Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
(`arm-none-eabi-gcc` in `PATH`, 14.2 used), CMake 3.21 or later, Ninja,
Python 3.

One build directory per board, with the boot loader and two applications
behind it (at 0x2000, see [docs/update.md](docs/update.md)):

```bash
cmake -S . -B build -G Ninja      # MPM v2 board
ninja -C build                    # boot loader, firmware and bench
ninja -C build flash-firmware     # J-Link on J10: boot loader and MPM firmware
ninja -C build flash-bench        # J-Link on J10: boot loader and radio bench
```

| Target | Files (in the build directory) |
|---|---|
| `bootloader` | `bootloader.elf`, `bootloader.bin` |
| `firmware` | the MULTI-Module core: `firmware.elf`, `firmware.bin` (with the image header), `mpm-v2.bin` (for the radio), `mpm-v2-full.bin` / `.hex` (the whole flash) |
| `bench` | the radio bench (`src/bench.c` command shell instead of the MPM, see [docs/bench.md](docs/bench.md)): `bench.elf`, `bench.bin`, `mpm-v2-bench.bin`, `mpm-v2-bench-full.bin` / `.hex` |
| `flash-firmware`, `flash-bench` | boot loader and application through the board's probe: J-Link on the MPM v2, XDS110 (OpenOCD) on the LaunchPad |

The CC2652R1 LaunchPad (bench console on UART0 at 921600 baud):

```bash
cmake -S . -B build-lp -G Ninja -DBOARD=CC2652R1_LAUNCHXL
ninja -C build-lp flash-bench
```

Host unit tests (a C compiler):

```bash
make -C tests/frame               # the framings
make -C tests/mpm                 # the MPM core, the log
make -C tests/storage             # littlefs on a simulated NOR flash, EEPROM, file server, settings
```

Options (`-D<option>=<value>`; the build type is Release unless
`CMAKE_BUILD_TYPE` says otherwise):

| Option | Description | Default |
|--------|-------------|---------|
| `BOARD` | Target board: `MPM_V2` (CC2652RB) or `CC2652R1_LAUNCHXL` (the LaunchPad). | `MPM_V2` |
| `DEBUG` | Debug output via Segger RTT as well (the USB log is always there). | OFF |
| `USE_CCFG` | Include the MCU user configuration (CCFG). | ON |
| `DRIVERLIB_NOROM` | Use the compiled driverlib instead of the ROM's. | OFF |
