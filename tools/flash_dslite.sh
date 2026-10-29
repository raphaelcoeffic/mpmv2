#!/bin/sh
# Flash a firmware with UniFlash's DSLite through an XDS110 (CC23xx/CC27xx
# LaunchPads: OpenOCD has no flash driver for them), then reset and run.
#
#   tools/flash_dslite.sh files...  .elf/.out/.hex, or .bin at an address
#   tools/flash_dslite.sh app.bin@0x0
#   tools/flash_dslite.sh -v app.bin@0x0     verify only, no erase or write
#
# Only the sectors the files cover are erased. CCFG and SCFG (0x4E020000,
# 0x4E040000) are kept unless a file has data there (a .bin at that address,
# an ELF or hex with a section there): then the files' CCFG is programmed.
#
# Environment:
#   DSLITE        dslite.sh (default: the newest UniFlash in /Applications/ti
#                 or ~/ti)
#   XDS_SERIAL    probe serial number, when several XDS110 are connected
#   XDS_DEVICE    UniFlash device name (default: cc2755p10)
#   OBJDUMP       for the CCFG check of ELF files (default: arm-none-eabi-objdump)

set -eu

verify_only=
if [ "${1:-}" = -v ]; then
  verify_only=1
  shift
fi

if [ $# -eq 0 ]; then
  echo "usage: $0 [-v] files... (.elf/.out/.hex, .bin@address)" >&2
  exit 1
fi

for f in "$@"; do
  if [ ! -f "${f%@*}" ]; then
    echo "${f%@*}: not found" >&2
    exit 1
  fi
  case "$f" in
    *.bin)
      echo "$f: a .bin needs an address (file.bin@0x...)" >&2
      exit 1 ;;
  esac
done

if [ -z "${DSLITE:-}" ]; then
  for d in /Applications/ti/uniflash_* "$HOME"/ti/uniflash_*; do
    [ -x "$d/dslite.sh" ] && DSLITE=$d/dslite.sh
  done
fi
if [ -z "${DSLITE:-}" ] || [ ! -x "$DSLITE" ]; then
  echo "dslite.sh not found: install UniFlash, or set DSLITE" >&2
  exit 1
fi

device=${XDS_DEVICE:-cc2755p10}
DEVICE=$(echo "$device" | tr a-z A-Z)

# Data in the CCFG or SCFG sector: 0x4E02xxxx-0x4E04xxxx
has_ccfg() {
  case "$1" in
    *@*)
      a=$(printf '%08x' "$(( ${1#*@} ))")
      case "$a" in 4e0[2-4]*) return 0 ;; esac
      return 1 ;;
    *.hex)
      grep -q -i '^:020000044E0[2-4]' "$1" ;;
    *)
      "${OBJDUMP:-arm-none-eabi-objdump}" -h "$1" |
        awk '$2 != "" && /^ *[0-9]+ / { getline f; if (f ~ /LOAD/) print $5 }' |
        grep -q -i '^4e0[2-4]' ;;
  esac
}

retain_ccfg=true
files=""
for f in "$@"; do
  has_ccfg "$f" && retain_ccfg=false
  case "$f" in
    *@*) files="$files ${f%@*},${f#*@}" ;;
    *) files="$files $f" ;;
  esac
done

dir=$(mktemp -d "${TMPDIR:-/tmp}/flash_dslite.XXXXXX")
trap 'rm -rf "$dir"' EXIT

# SWD (the CC27xx targetdb default isn't applied without it), the Aux COM
# port as the target UART (the LaunchPad's back channel)
if [ -n "${XDS_SERIAL:-}" ]; then
  probe="<property Type=\"choicelist\" Value=\"1\" id=\"Debug Probe Selection\">
                <choice Name=\"Select by serial number\" value=\"0\">
                    <property Type=\"stringfield\" Value=\"$XDS_SERIAL\" id=\"-- Enter the serial number\"/>
                </choice>
            </property>"
else
  probe="<property Type=\"choicelist\" Value=\"0\" id=\"Debug Probe Selection\"/>"
fi

cat > "$dir/target.ccxml" <<EOF
<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<configurations XML_version="1.2" id="configurations_0">
    <configuration XML_version="1.2" id="configuration_0">
        <instance XML_version="1.2" desc="Texas Instruments XDS110 USB Debug Probe" href="connections/TIXDS110_Connection.xml" id="Texas Instruments XDS110 USB Debug Probe" xml="TIXDS110_Connection.xml" xmlpath="connections"/>
        <connection XML_version="1.2" id="Texas Instruments XDS110 USB Debug Probe">
            <instance XML_version="1.2" href="drivers/tixds510cs_dap.xml" id="drivers" xml="tixds510cs_dap.xml" xmlpath="drivers"/>
            <instance XML_version="1.2" href="drivers/tixds510cortexM33.xml" id="drivers" xml="tixds510cortexM33.xml" xmlpath="drivers"/>
            <instance XML_version="1.2" href="drivers/tixds510sec_ap.xml" id="drivers" xml="tixds510sec_ap.xml" xmlpath="drivers"/>
            $probe
            <property Type="choicelist" Value="1" id="SWD Mode Settings">
                <choice Name="SWD Mode - Aux COM port is target UART port" value="nothing"/>
            </property>
            <platform XML_version="1.2" id="platform_0">
                <instance XML_version="1.2" desc="$DEVICE" href="devices/$device.xml" id="$DEVICE" xml="$device.xml" xmlpath="devices"/>
            </platform>
        </connection>
    </configuration>
</configurations>
EOF

# dslite.sh passes its arguments through an unquoted eval: no spaces in paths
if [ -n "$verify_only" ]; then
  "$DSLITE" --config="$dir/target.ccxml" -v $files
else
  "$DSLITE" --config="$dir/target.ccxml" \
    -s RetainMainSectors=true -s RetainCcfgScfg=$retain_ccfg \
    -s VerifyAfterProgramLoad=0 \
    -f $files -r 0
fi
