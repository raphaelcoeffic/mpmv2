#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""Two-way link test between the MPM and the CC2652: numbered packets, all bytes
compared (the counter is at byte 2), CRC flags checked.

  link.py --mpm "preset hubsan" --cc "trim 90;preset hubsan" --len 16 --ch 170
  link.py --mpm frskyd.txt --cc cc2652_frskyd.txt --payload 11a5e01b... --cc2500
  link.py --mpm nrf_1m.txt --cc cc2652_nrf_1m.txt --len 16 --dir rev

Setups: ';'-separated commands and example files, see rig.py.
"""

import argparse
import sys

import rig


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mpm", required=True, help="MPM setup")
    ap.add_argument("--cc", required=True, help="CC2652 setup")
    ap.add_argument("--mpm-rx", help="added to the MPM setup to receive (rev)")
    ap.add_argument("--cc-rx", help="added to the CC2652 setup to send (rev)")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--payload", help="hex payload (CC2500 variable length: starts with the length byte)")
    g.add_argument("--len", type=int, help="payload of this length (rig.pattern())")
    ap.add_argument("--ch", type=int, help="channel, appended to both setups")
    ap.add_argument("--mpm-ch", type=int, help="MPM channel if different")
    ap.add_argument("--seq", type=int, default=rig.SEQ, help="payload byte replaced by the counter")
    ap.add_argument("--cc2500", action="store_true",
                    help="MPM CC2500 receiving: one packet per FIFO read, CRC_OK checked")
    rig.common_args(ap, period=10000, count=200)
    a = ap.parse_args()

    payload = bytes.fromhex(a.payload) if a.payload else rig.pattern(a.len)
    mpm, cc = rig.open_devices()
    with mpm, cc:
        r = rig.link_test(mpm, cc, payload, mpm_setup=a.mpm, cc_setup=a.cc, mpm_rx_setup=a.mpm_rx,
                          cc_rx_setup=a.cc_rx, ch=a.ch, mpm_ch=a.mpm_ch, period=a.period, count=a.count,
                          direction=a.dir, cc2500=a.cc2500, seq=a.seq, verbose=a.verbose)
    rig.print_results("link", r)
    return 0 if all(v["good"] == v["count"] for v in r.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
