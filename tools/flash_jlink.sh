#!/bin/sh
# Flash a firmware with a J-Link (MPM v2 J10, or a LaunchPad), then reset and run.
#
#   tools/flash_jlink.sh [firmware.elf]     default: build/firmware.elf
#
# Environment:
#   JLINK_DEVICE  J-Link device name (default: CC2652RB)
#   JLINK_SPEED   interface speed in kHz (default: 4000)
#   JLINK_SERIAL  probe serial number, when several probes are connected
#
# Accepts what J-Link Commander's loadfile accepts (.elf, .hex, .bin at 0).

set -eu

fw=${1:-build/firmware.elf}
if [ ! -f "$fw" ]; then
  echo "$fw: not found" >&2
  exit 1
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

JLinkExe -NoGui 1 -ExitOnError 1 \
  -Device "${JLINK_DEVICE:-CC2652RB}" -If JTAG -Speed "${JLINK_SPEED:-4000}" \
  ${JLINK_SERIAL:+-SelectEmuBySN "$JLINK_SERIAL"} \
  -AutoConnect 1 -CommandFile "$script"
