#!/bin/sh
# Flash a firmware with a J-Link (MPM v2 J10, or a LaunchPad), then reset and run.
#
#   tools/flash_jlink.sh [firmware.elf]     default: build/firmware.elf
#   tools/flash_jlink.sh build-bl/bootloader.elf build-bl/firmware.bin@0x2000
#                                           several files, .bin at an address
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

[ $# -gt 0 ] || set -- build/firmware.elf

for f in "$@"; do
  if [ ! -f "${f%@*}" ]; then
    echo "${f%@*}: not found" >&2
    exit 1
  fi
done

speed=${JLINK_SPEED:-4000}

if [ "${JLINK_TOOL:-openocd}" = openocd ]; then
  cmds=""
  n=$#
  i=0
  for f in "$@"; do
    i=$((i + 1))
    case "$f" in
      *@*) img="{${f%@*}} ${f#*@}" ;;
      *) img="{$f}" ;;
    esac
    [ $i -lt $n ] || img="$img verify reset exit"
    [ $i -eq $n ] || img="$img verify"
    cmds="$cmds -c \"program $img\""
  done
  eval openocd -f interface/jlink.cfg \
    ${JLINK_SERIAL:+-c \"adapter serial $JLINK_SERIAL\"} \
    -c \"transport select jtag\" -c \"adapter speed $speed\" \
    -f target/ti_cc26x2.cfg $cmds
  exit
fi

script=$(mktemp "${TMPDIR:-/tmp}/flash_jlink.XXXXXX")
trap 'rm -f "$script"' EXIT

{
  echo r
  for f in "$@"; do
    case "$f" in
      *@*) echo "loadfile \"${f%@*}\" ${f#*@}" ;;
      *) echo "loadfile \"$f\"" ;;
    esac
  done
  echo r
  echo g
  echo exit
} > "$script"

# -JTAGConf: otherwise the JTAG chain position prompt eats the script
JLinkExe -NoGui 1 -ExitOnError 1 \
  -Device "${JLINK_DEVICE:-CC2652RB}" -If JTAG -Speed "$speed" -JTAGConf -1,-1 \
  ${JLINK_SERIAL:+-SelectEmuBySN "$JLINK_SERIAL"} \
  -AutoConnect 1 -CommandFile "$script"
