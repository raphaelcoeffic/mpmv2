#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""A7105 family: the MPM A7105 with each protocol's A7105_Init() table (bench
preset of the same name) against the CC2652 preset, both ways.

  suite_a7105.py                 all, ch 170
  suite_a7105.py flysky --ch 20

Most of these protocols only transmit and their tables don't receive: for
CC2652 -> MPM the MPM uses the AFHDS2A table set to the protocol's DATA_RATE
(0x0E), CODE_I (0x1F), FIFO length (0x03) and ID. Bugs too: Bugs_a7105.ino receives its
telemetry at PLL_I - 2 with its own table, which receives nothing on the bench.
--trim: this A7105 is about 90 kHz above the CC2652.
"""

import argparse
import sys

import rig

# name: (payload length, ID, DATA_RATE, CODE_I), None: the ID / rate of the preset
P = {
    "flysky": (21, None, 0, 0x0F),
    "afhds2a": (38, None, 0, 0x1F),
    "hubsan": (16, None, 4, 0x07),
    "bugs": (22, "5475c52a", 1, 0x0F),
    "height": (8, "25a53c45", 1, 0x1F),
    "height8": (11, "25a53c45", 1, 0x1F),
    "pelikan": (16, "06230623", 1, 0x07),
    "kyosho": (38, "5475c52a", 0, 0x1F),
    "kyoshohype": (17, "5475c52a", 1, 0x1F),
    "wfly2": (32, "5475c52a", 0, 0x0F),
    "joysway": (16, "5475c52a", 0, 0x0F),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", help=f"protocols (default: all): {' '.join(P)}")
    ap.add_argument("--ch", type=int, default=170, help="A7105 PLL_I channel (default 170: 2485 MHz)")
    ap.add_argument("--trim", type=int, default=90, help="CC2652 trim in kHz (default 90)")
    rig.common_args(ap, period=5000, count=500)
    a = ap.parse_args()
    fails = 0
    mpm, cc = rig.open_devices()
    with mpm, cc:
        for name in a.names or list(P):
            n, id_, rate, code = P[name]
            payload = rig.pattern(n)
            cc_setup = f"trim {a.trim};preset {'height' if name == 'height8' else name}"
            mpm_tx = f"preset {name}"
            if id_:
                cc_setup += f";a7105 {id_} {int(bool(code & 0x08))} {int(bool(code & 0x10))}"
                mpm_tx += f";w 0x06 {id_}"
            if name in ("hubsan", "afhds2a", "wfly2", "joysway"):  # their tables receive
                mpm_rx = mpm_tx
            else:
                mpm_rx = (f"preset afhds2a;w 0x0E {rate:#04x};w 0x1F {code:#04x};w 0x03 {n - 1:#04x}"
                          + (f";w 0x06 {id_}" if id_ else ""))
            r = {}
            if a.dir in ("both", "fwd"):
                r |= rig.link_test(mpm, cc, payload, mpm_setup=mpm_tx, cc_setup=cc_setup, ch=a.ch,
                                   period=a.period, count=a.count, direction="fwd", verbose=a.verbose)
            if a.dir in ("both", "rev"):
                r |= rig.link_test(mpm, cc, payload, mpm_setup=mpm_rx, cc_setup=cc_setup, ch=a.ch,
                                   period=a.period, count=a.count, direction="rev", verbose=a.verbose)
            rig.print_results(name, r)
            fails += sum(v["good"] < v["count"] for v in r.values())
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
