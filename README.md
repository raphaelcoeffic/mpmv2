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

Credits: the RF core work builds on the 38C3 talk "Beyond BLE: Cracking open
the black-box of RF microcontrollers" by Adam Batori and Robert Pafford
([talk](https://events.ccc.de/congress/2024/hub/en/event/beyond-ble-cracking-open-the-black-box-of-rf-microcontrollers/),
[tools](https://github.com/rjp5th/beyond-ble-tools)): thanks for the ideas
and the inspiration.

Radio bench build (LaunchPad, `src/bench.c` command shell on UART0 at 921600
baud instead of the MPM):

```bash
cmake -S . -B build-bench -G Ninja -DBOARD_CC2652R1_LAUNCHXL=ON -DBENCH=ON -DCMAKE_BUILD_TYPE=Release
ninja -C build-bench
openocd -f board/ti_cc26x2_launchpad.cfg -c "program build-bench/firmware.elf verify reset exit"
make -C tests/frame    # host unit tests of the framings
```

On the MPM v2 board (console on the USB-C port, J-Link on J10):

```bash
cmake -S . -B build-v2 -G Ninja -DBENCH=ON -DCMAKE_BUILD_TYPE=Release
ninja -C build-v2 flash
```

# Compilation Instructions

This README provides instructions for compiling the project using CMake with the
available configuration options.

## Prerequisites

- CMake (version 3.x or later)
- [ARM GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)

## Configuration Options

The project supports the following configuration options that can be enabled or
disabled during the build process:

| Option | Description | Default |
|--------|-------------|---------|
| `BOARD_CC2652R1_LAUNCHXL` | Set target board to CC2652R1-LAUNCHXL. If OFF, MPM with CC2652RB is assumed. | OFF |
| `DEBUG` | Enable debug output via Segger RTT. | OFF |
| `USE_CCFG` | Include MCU user configuration (CCFG). | ON |
| `USE_XOSC` | Use external oscillator (or HPOSC on CC2652RB). | ON |
| `DRIVERLIB_NOROM` | Use compiled driverlib instead of ROM. | OFF |
| `BENCH` | Radio bench mode: test shell on UART0 instead of the MPM (see `docs/bench.md`). | OFF |
| `BOOTLOADER` | Boot loader at 0x0 (update from the radio), application at 0x2000 (see `docs/update.md`). | OFF |

## Building the Project

### Basic Build

```bash
# Create and navigate to build directory
mkdir build
cd build

# Generate build files with default configuration
cmake ..

# Compile the project
cmake --build .
```

### Customizing Build Options

To customize build options, you can use the `-D` flag with CMake:

```bash
# Example: Enable debug output and target the CC2652R1 LaunchXL board
cmake -DDEBUG=ON -DBOARD_CC2652R1_LAUNCHXL=ON ..

# Example: Disable CCFG and use compiled driverlib
cmake -DUSE_CCFG=OFF -DDRIVERLIB_NOROM=ON ..
```

## Examples

### Building for CC2652R1 LaunchXL with Debug

```bash
cmake -DBOARD_CC2652R1_LAUNCHXL=ON -DDEBUG=ON ..
cmake --build .
```

### Building for MPM with CC2652RB (Default)

```bash
# No need to specify BOARD_CC2652R1_LAUNCHXL as it's OFF by default
cmake ..
cmake --build .
```

### Advanced: Disabling External Oscillator

```bash
cmake -DUSE_XOSC=OFF ..
cmake --build .
```

## Notes

- When `BOARD_CC2652R1_LAUNCHXL` is OFF, the build targets an MPM board with CC2652RB
- When using CC2652RB, the `USE_XOSC` option will configure HPOSC instead of an external oscillator
- Enabling `DEBUG` will configure Segger RTT for debug output
- The `DRIVERLIB_NOROM` option determines whether to use the compiled driverlib or the ROM version
