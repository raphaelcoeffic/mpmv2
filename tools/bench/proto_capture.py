#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""The MPM runs one of its protocols ("proto"), the CC2652 receives on one
channel: packets, CRC results, most frequent payloads.

  proto_capture.py e012              a known case (see CASES)
  proto_capture.py frskyl --secs 5
  proto_capture.py e010r5 --seq      ok/bad sequence with the time gaps (* = bad)
  proto_capture.py --proto "81" --cc "preset gfsk1m;rf2500 0e5496ee 0;ch 56" --rx 14

E012: most CRCs are wrong (MPM HS6200_calc_crc() bug, docs/interop-results.md).
FrSky L: the packet CRC (FrSkyX_crc(), bind: init 0, data: init RX_num) is
checked here, the CC2652 framing has none.
"""

import argparse
import sys
import time
from collections import Counter

import rig

# name: (MPM proto args, CC2652 setup, rx length)
CASES = {
    "e012": ("45 0", "cc2652_e012.txt", 15),
    "e015": ("45 1", "preset gfsk1m;hs6200 6254793853;trim 0;ch 45", 10),
    "e129": ("83 0", "cc2652_e129.txt", 16),
    "c186": ("83 1", "cc2652_e129.txt", 19),
    "e010r5": ("81", "preset gfsk1m;rf2500 0e5496ee 0;trim 0;ch 48", 14),
    "frskyl": ("67 0 0 0 1", "cc2652_frskyl.txt", 17),
}

_T = [0x0000, 0x1189, 0x2312, 0x329B, 0x4624, 0x57AD, 0x6536, 0x74BF,
      0x8C48, 0x9DC1, 0xAF5A, 0xBED3, 0xCA6C, 0xDBE5, 0xE97E, 0xF8F7]


def frskyx_crc(data: bytes, init: int = 0) -> int:
    c = init
    for b in data:
        v = ((c >> 8) ^ b) & 0xFF
        c = ((c << 8) ^ _T[v & 0x0F] ^ (0x1081 * (v >> 4))) & 0xFFFF
    return c


def frskyl_ok(p: bytes) -> bool:
    return len(p) >= 17 and (p[15] << 8 | p[16]) in {frskyx_crc(p[1:15], n) for n in range(64)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("case", nargs="?", choices=CASES)
    ap.add_argument("--proto", help="MPM proto arguments: <num> [sub] [rxnum] [option] [bind]")
    ap.add_argument("--cc", help="CC2652 setup")
    ap.add_argument("--rx", type=int, help="CC2652 rx length")
    ap.add_argument("--mpm", help="MPM commands before proto (e.g. chan values)")
    ap.add_argument("--secs", type=float, default=2.0)
    ap.add_argument("--seq", action="store_true", help="print the ok/bad sequence with the time gaps")
    ap.add_argument("--top", type=int, default=8, help="most frequent payloads to print")
    a = ap.parse_args()
    proto, cc_setup, rx_len = CASES[a.case] if a.case else (None, None, None)
    proto, cc_setup, rx_len = a.proto or proto, a.cc or cc_setup, a.rx or rx_len
    if not (proto and cc_setup and rx_len):
        ap.error("give a case, or --proto, --cc and --rx")

    mpm, cc = rig.open_devices()
    with mpm, cc:
        mpm.cmd("stop")
        rig.run(mpm, a.mpm)
        rig.run(cc, cc_setup)
        rig.drain(cc)
        print("cc2652 rx:", cc.cmd(f"rx {rx_len}"))
        print("mpm proto:", mpm.cmd(f"proto {proto}"))
        time.sleep(a.secs)
        print("cc2652", cc.cmd("stop"))
        print("mpm", mpm.cmd("stop"))
        lines = rig.events(cc)

    res, payloads, seq, prev = Counter(), Counter(), [], None
    for e in lines:
        p = e.split()
        data = bytes.fromhex(p[3]) if "=" not in p[3] else b""
        ok = frskyl_ok(data) if proto.split()[0] == "67" else rig.flag(e, "crc") != "bad"
        res["crc ok" if ok else "crc bad"] += 1
        payloads[f"{p[3]} {'ok' if ok else 'BAD'}"] += 1
        t = int(p[1])
        seq.append(f"{'' if prev is None else t - prev}{'' if ok else '*'}")
        prev = t
    print(f"{len(lines)} packets: {dict(res)}")
    if a.seq:
        print(" ".join(seq))
    for k, v in payloads.most_common(a.top):
        print(f"{v:5d} {k}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
