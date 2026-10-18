#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""CYRF6936 (DSSS) family: the MPM CYRF6936 with DSM / Devo setups (examples)
and with the register setups of the other protocols (the bind configuration of
each *_cyrf6936.ino, written with "w"), against the CC2652 "cyrf" framing, both
ways, ch 30.

  suite_cyrf.py                all
  suite_cyrf.py dsm mlink

Measured: the CYRF6936 sends at 2400 + n MHz, 2401 + n with TX_OFFSET 0x0555
(most protocols), and receives at about 2401.2 + n with RX_CFG FAST_TURN_EN
(bit 3) and bit 4 clear, at 2402 + n otherwise: some reverse runs move the
CC2652 channel plan. The MPM receiving with a TX-only setup needs some help
(the 4th entry): CLK_EN 0x02, RX CRC check off...
"""

import argparse
import sys

import rig

D = "6fbe3201dbf12b01e35cfa029793f902"        # CYRF6936 default data codes
DEF_SOP = "82c79036219eff17"                  # CYRF6936 default SOP code
J6PRO_D = "02f9939702fa5ce3012bf1db0132be6f"  # j6pro_data_code
DSM_BIND = "d7a154b15e89ae86"
SOP = ["3C37CC91E2F8CC91", "9BC5A10FAD39A20F", "EF64B02AD28FB12A", "66CD7C50DD267C50",
       "5CE1F644AD16F644", "5ACCAE46B631AE46", "A178DC3C9E82DC3C", "B98E19746F651874",
       "DFB1C04962DFC149", "97E514727F1A1472", "82C7903621 03FF17".replace(" ", ""),
       "E2F8CC913C37CC91", "AD39A20F9BC5A10F", "D28FB12AEF64B02A", "DD267C5066CD7C50",
       "AD16F6445CE1F644", "B631AE465ACCAE46", "9E82DC3CA178DC3C", "6F651874B98E1974",
       "62DFC149DFB1C049"]
# name: (MPM setup, CC2652 setup, payload length, [MPM RX extra, [CC2652 TX extra]])
P = {
    "dsm": ("dsm.txt", "cc2652_dsm.txt", 16),
    "dsmbind": ("dsmbind.txt", "cc2652_dsmbind.txt", 16),
    # bind response: the first 4 bytes are known to the TX (prefix), the counter after them
    "dsmbindresp": ("dsm_bindresp.txt", f"cc2652_dsm_bindresp.txt;prefix {rig.pattern(4).hex()}", 10),
    "devo": ("devo.txt", "cc2652_devo.txt", 16),
    "j6pro": ("chip cyrf;w 0x28 0x02;w 0x32 0x3c;w 0x35 0x14;w 0x1c 0x05;w 0x1b 0x55;w 0x06 0x4a;w 0x03 0x28;"
              f"w 0x12 0x0e;w 0x10 0xee;w 0x1f 0x00;w 0x1e 0x00;w 0x23 {J6PRO_D};w 0x24 023333;"
              f"w 0x22 {SOP[19]};w 0x15 0x00;w 0x16 0x00",
              f"preset dsm;cyrf {SOP[19]} {J6PRO_D} 0 1 1 0 64", 16),
    "wk2x01": ("chip cyrf;w 0x03 0x28;w 0x06 0x4A;w 0x0B 0x00;w 0x0C 0xC0;w 0x0D 0x04;w 0x0F 0x2C;w 0x10 0xEE;"
               "w 0x1B 0x55;w 0x1C 0x05;w 0x1D 0x18;w 0x32 0x3C;w 0x35 0x14;w 0x1E 0x90;w 0x1F 0x00;"
               "w 0x01 0x10;w 0x0F 0x2C;w 0x28 0x02;w 0x27 0x02;w 0x22 DFB1C04962DFC149;w 0x0F 0x28;"
               "w 0x1E 0x10;w 0x0E 0x20;w 0x0F 0x2C",
               f"preset dsm;cyrf DFB1C04962DFC149 {D} 0 1 1 0 64", 16),
    "scorpio": ("chip cyrf;w 0x32 0x3C;w 0x35 0x14;w 0x1B 0x55;w 0x1C 0x05;w 0x10 0xE8;w 0x03 0x28;"
                "w 0x15 0x01;w 0x16 0x00",
                f"preset dsm;cyrf {DEF_SOP} {D} 1 1 1 0 64", 8, "w 0x28 0x02", "chplan 2402000000 1000000"),
    "kyosho3": ("chip cyrf;w 0x0B 0x00;w 0x32 0x3C;w 0x35 0x14;w 0x03 0x28;w 0x10 0xA4;w 0x1F 0x05;w 0x1E 0x04;"
                "w 0x12 0x0E;w 0x06 0x52;w 0x24 043333",
                f"preset dsm;chplan 2400000000 1000000;cyrf {DEF_SOP[:8]} {D} 0 0 1 0 64 1", 9,
                "w 0x28 0x02;w 0x39 0x03", "chplan 2402000000 1000000"),
    "traxxas": ("chip cyrf;w 0x32 0x3C;w 0x35 0x14;w 0x1B 0x55;w 0x1C 0x05;w 0x28 0x02;w 0x03 0x08;w 0x0B 0x00;"
                f"w 0x06 0x8A;w 0x1E 0x08;w 0x22 {SOP[0]};w 0x15 0x5A;w 0x16 0x5A",
                f"preset devo;cyrf {SOP[0][:8]} {D} 0x5a5a 1 1 0 32", 16),
    "wfly": ("chip cyrf;w 0x1D 0x19;w 0x32 0x3C;w 0x35 0x14;w 0x1B 0x55;w 0x1C 0x05;w 0x06 0x4A;w 0x10 0xE8;"
             "w 0x03 0x08;w 0x0C 0xC4;w 0x0D 0x04;w 0x0F 0x21;w 0x1E 0x00;w 0x15 0x00;w 0x16 0x00;"
             f"w 0x22 {SOP[5]}",
             f"preset devo;cyrf {SOP[5]} {D} 0 1 1 0 32", 16, "w 0x28 0x02;rx 16;stop"),  # 1st rx gets nothing
    # MLINK sets RX_CFG VLD_EN (a valid flag byte after each byte): off to receive.
    # The reverse needs inverted chips from the CC2652 (not understood)
    "mlink": ("chip cyrf;w 0x01 0x08;w 0x02 0x40;w 0x03 0x3C;w 0x05 0x00;w 0x06 0x93;w 0x0B 0x00;w 0x0F 0x04;"
              "w 0x10 0x00;w 0x11 0x05;w 0x12 0x0F;w 0x14 0x05;w 0x15 0x00;w 0x16 0x00;w 0x1B 0x00;"
              "w 0x1C 0x00;w 0x1D 0x00;w 0x1E 0x14;w 0x1F 0x04;w 0x26 0x08;w 0x29 0x00;w 0x32 0x3C;"
              f"w 0x35 0x14;w 0x39 0x03;w 0x24 043333;w 0x23 {D}",
              f"preset dsmbind;chplan 2400000000 1000000;cyrf 0 {D[:16]} 0 0 0 1", 8,
              "w 0x28 0x02;w 0x06 0x92", f"cyrf 0 {D[:16]} 0 0 0 1 64 1;chplan 2402000000 1000000"),
    "losi": ("chip cyrf;w 0x28 0x02;w 0x32 0x3C;w 0x35 0x14;w 0x06 0x48;w 0x1B 0x55;w 0x1C 0x05;w 0x03 0x38;"
             f"w 0x12 0x0A;w 0x39 0x01;w 0x24 043333;w 0x10 0x4A;w 0x1F 0x04;w 0x23 {DSM_BIND}{DSM_BIND}",
             f"preset dsmbind;cyrf 0 {DSM_BIND} 0 0 0 1", 10, "w 0x1E 0x14"),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", help=f"protocols (default: all): {' '.join(P)}")
    ap.add_argument("--ch", type=int, default=30, help="channel (default 30)")
    rig.common_args(ap, period=20000, count=100)
    a = ap.parse_args()
    fails = 0
    mpm, cc = rig.open_devices()
    with mpm, cc:
        for name in a.names or list(P):
            mpm_setup, cc_setup, n, mpm_rx, cc_tx = (P[name] + ("", ""))[:5]
            r = rig.link_test(mpm, cc, rig.pattern(n), mpm_setup=mpm_setup, cc_setup=f"trim 0;{cc_setup}",
                              mpm_rx_setup=mpm_rx, cc_rx_setup=cc_tx, ch=a.ch, period=a.period,
                              count=a.count, direction=a.dir, seq=5 if name == "dsmbindresp" else rig.SEQ,
                              verbose=a.verbose)
            rig.print_results(name, r)
            fails += sum(v["good"] < v["count"] for v in r.values())
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
