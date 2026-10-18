#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""Raw capture: the MPM sends (tx, or a protocol), the CC2652 receives the raw
bits after a sync word of its choice ("sync": no framing). How the on-air
formats in docs/interop-results.md were worked out: pick a PHY and a sync word
(e.g. the end of the preamble), capture, compare with the chip's datasheet.

  raw_capture.py --mpm "preset afhds2a;ch 20" --tx 0102030405 \\
      --cc "phy 500000 250000 97 4;sync aaaa5475;freq 2410000" --rx 60
  raw_capture.py --mpm cyrf_gfsk1m.txt --tx <frame, MSB first> --lsb --cc "preset gfsk1m;sync aaaa..." --rx 20
  raw_capture.py --proto "45 0" --cc "preset gfsk1m;sync 55429c8f;ch 60" --rx 30 --secs 1

Prints each capture once with its count, most frequent first, and the RSSI.
"""

import argparse
import sys
import time
from collections import Counter

import rig


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mpm", help="MPM setup")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--tx", help="hex packet the MPM sends")
    g.add_argument("--proto", help="MPM protocol to run (proto arguments)")
    ap.add_argument("--lsb", action="store_true", help="bit-reverse each --tx byte (CYRF6936 GFSK mode)")
    ap.add_argument("--cc", required=True, help="CC2652 setup: PHY, sync word, channel / freq")
    ap.add_argument("--rx", type=int, required=True, help="bytes to capture after the sync word")
    ap.add_argument("--n", type=int, default=10, help="packets to send with --tx (default 10)")
    ap.add_argument("--period", type=float, default=0.02, help="seconds between --tx packets")
    ap.add_argument("--secs", type=float, default=1.0, help="capture time with --proto")
    a = ap.parse_args()
    mpm, cc = rig.open_devices()
    with mpm, cc:
        mpm.cmd("stop")
        rig.run(mpm, a.mpm)
        rig.run(cc, a.cc)
        rig.drain(cc)
        print("cc2652:", cc.cmd(f"rx {a.rx}"))
        if a.tx:
            data = bytes.fromhex(a.tx)
            if a.lsb:
                data = bytes(int(f"{b:08b}"[::-1], 2) for b in data)
            for _ in range(a.n):
                mpm.cmd(f"tx {data.hex()}")
                time.sleep(a.period)
        else:
            print("mpm:", mpm.cmd(f"proto {a.proto}"))
            time.sleep(a.secs)
            mpm.cmd("stop")
        time.sleep(0.2)
        print("cc2652:", cc.cmd("stop"))
        caps, rssi = Counter(), {}
        for e in rig.events(cc):
            caps[e.split()[3]] += 1
            rssi[e.split()[3]] = rig.flag(e, "rssi")
    for k, v in caps.most_common():
        print(f"{v:4d} {k} rssi={rssi[k]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
