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
