#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""Frequency scan: the CC2652 receives (fwd) or sends (rev) at several offsets
from a centre frequency, 10 packets each. Finds where a chip really sends or
receives, and the frequency offset between the two boards.

  freqscan.py --mpm "chip cyrf;..." --cc "preset dsm" --len 16 --center 2431000 -500 -250 0 250 500
  freqscan.py --dir rev --mpm hubsan.txt --cc cc2652_hubsan.txt --len 16 --center 2410000 0 60 120

Offsets in kHz; --center is the CC2652 frequency in kHz ("freq"). The MPM
channel is part of its setup. Narrow the CC2652 filter (phy) to measure an
offset precisely.
"""

import argparse
import sys
import time

import rig


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("offsets", nargs="+", type=int, help="kHz")
    ap.add_argument("--mpm", required=True, help="MPM setup")
    ap.add_argument("--cc", required=True, help="CC2652 setup")
    ap.add_argument("--len", type=int, required=True, help="payload length")
    ap.add_argument("--center", type=int, required=True, help="CC2652 centre frequency in kHz")
    ap.add_argument("--dir", default="fwd", choices=("fwd", "rev"), help="fwd: MPM -> CC2652")
    a = ap.parse_args()
    payload = rig.pattern(a.len).hex()
    mpm, cc = rig.open_devices()
    with mpm, cc:
        rig.run(mpm, a.mpm)
        rig.run(cc, f"trim 0;{a.cc}")
        tx, rx = (mpm, cc) if a.dir == "fwd" else (cc, mpm)
        for off in a.offsets:
            cc.cmd(f"freq {a.center + off}")
            rig.drain(rx)
            rx.cmd(f"rx {a.len}")
            for _ in range(10):
                tx.cmd(f"tx {payload}")
                time.sleep(0.02)
            time.sleep(0.1)
            rx.cmd("stop")
            ev = rig.events(rx)
            good = sum(e.split()[3].startswith(payload) and rig.flag(e, "crc") != "bad" for e in ev)
            print(f"{off:+6d} kHz: {len(ev)} received, {good}/10 right", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
