#!/usr/bin/env bash
# Build (and optionally flash) the MULTI-Module bench firmware, branch mpm-bench
# of https://github.com/raphaelcoeffic/DIY-Multiprotocol-TX-Module
#   build_mpm.sh                 build with the toolchain of the board package
#   build_mpm.sh --native        build with the arm-none-eabi-gcc found in PATH
#   build_mpm.sh --upload        build and flash through the STM32duino bootloader (USB)
# MPM_DIR: the DIY-Multiprotocol-TX-Module checkout (default: next to mpm-v2)
# FQBN: other board, must be an STM32 debug option. PORT: serial port for --upload
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
MPM_DIR="${MPM_DIR:-$(cd "$HERE/../../.." && pwd)/DIY-Multiprotocol-TX-Module}"
FQBN="${FQBN:-multi4in1:STM32F1:multistm32f103cb:debug_option=native}"
BUILD="${BUILD:-$HERE/build}"
ARGS=(--build-property "compiler.cpp.extra_flags=-DMULTI_BENCH")
UPLOAD=0

for arg in "$@"; do
	case "$arg" in
		--native) ARGS+=(--build-property "compiler.path=$(dirname "$(command -v arm-none-eabi-gcc)")/") ;;
		--upload) UPLOAD=1 ;;
		*) echo "unknown option $arg" >&2; exit 2 ;;
	esac
done

if [ ! -f "$MPM_DIR/Multiprotocol/_Bench.ino" ]; then
	echo "$MPM_DIR has no bench firmware: set MPM_DIR to a checkout of the mpm-bench branch" >&2
	exit 1
fi

arduino-cli compile -b "$FQBN" "$MPM_DIR/Multiprotocol/Multiprotocol.ino" --build-path "$BUILD" "${ARGS[@]}"
cp "$BUILD/Multiprotocol.ino.bin" "$BUILD/multi-bench.bin"
echo "Firmware: $BUILD/multi-bench.bin"

if [ "$UPLOAD" = 1 ]; then
	# The maple uploader resets the module through its USB serial port when one exists,
	# otherwise the module must already be in its bootloader (DFU 1eaf:0003).
	PORT="${PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | grep -v usbmodemL | head -1 || true)}"
	arduino-cli upload -b "$FQBN" --input-dir "$BUILD" -p "${PORT:-/dev/null}"
fi
