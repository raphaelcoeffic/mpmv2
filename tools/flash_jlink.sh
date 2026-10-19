#!/bin/sh
# Flash a firmware with a J-Link (MPM v2 J10, or a LaunchPad), then reset and run.
#
#   tools/flash_jlink.sh [firmware.elf]     default: build/firmware.elf
#
# Uses OpenOCD (libjaylink, no SEGGER DLL: no J-Link EDU terms of use pop-up),
# or J-Link Commander with JLINK_TOOL=segger.
#
# Environment:
#   JLINK_TOOL    openocd (default) or segger
#   JLINK_SPEED   interface speed in kHz (default: 4000)
#   JLINK_SERIAL  probe serial number, when several probes are connected
#   JLINK_DEVICE  J-Link Commander device name (default: CC2652RB)

set -eu

fw=${1:-build/firmware.elf}
if [ ! -f "$fw" ]; then
  echo "$fw: not found" >&2
  exit 1
fi

speed=${JLINK_SPEED:-4000}

if [ "${JLINK_TOOL:-openocd}" = openocd ]; then
  openocd -f interface/jlink.cfg \
    ${JLINK_SERIAL:+-c "adapter serial $JLINK_SERIAL"} \
    -c "transport select jtag" -c "adapter speed $speed" \
    -f target/ti_cc26x2.cfg \
    -c "program {$fw} verify reset exit"
  exit
fi

script=$(mktemp "${TMPDIR:-/tmp}/flash_jlink.XXXXXX")
trap 'rm -f "$script"' EXIT

cat > "$script" <<EOF
r
loadfile "$fw"
r
g
exit
EOF

# -JTAGConf: otherwise the JTAG chain position prompt eats the script
JLinkExe -NoGui 1 -ExitOnError 1 \
  -Device "${JLINK_DEVICE:-CC2652RB}" -If JTAG -Speed "$speed" -JTAGConf -1,-1 \
  ${JLINK_SERIAL:+-SelectEmuBySN "$JLINK_SERIAL"} \
  -AutoConnect 1 -CommandFile "$script"
