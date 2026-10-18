#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""FrSky L, CC2652 -> MPM CC2500: the CC2652 sends the chip stream ("preset
frskyl"), the CC2500 can't receive it whole (64 byte FIFO, no sync word in the
stream). It receives 60 byte chunks after a 16 bit sync word taken from the
stream (examples/frskyl_rx.txt) with RXOFF_MODE RX, so it syncs again at the
next match in the same stream. Each chunk is placed at its best sync match in
the expected stream; prints the bit errors per position and the payload chips
covered by exact chunks.

  frskyl_reverse.py                          sync de25 (start of the payload)
  frskyl_reverse.py --sync de25 --sync 712e  coverage of several runs
"""

import argparse
import random
import sys
import time
from collections import Counter

import rig

HDR = 32  # 0x55 preamble bits sent by the CC2652 before the stream (4 bytes)


def chips(data: bytes) -> list[int]:
    out = []
    for b in data:
        for j in range(8):
            c = 0x712 if (b >> j) & 1 else 0x0ED
            out += [(c >> (10 - k)) & 1 for k in range(11)]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sync", action="append", help="16 bit CC2500 sync word(s), hex (default de25)")
    ap.add_argument("--ch", type=int, default=0)
    ap.add_argument("--count", type=int, default=50)
    ap.add_argument("--trim", type=int, default=-35, help="CC2652 trim in kHz (default -35)")
    a = ap.parse_args()

    random.seed(7)
    payload = bytes([0x4E] + [random.randrange(256) for _ in range(16)])
    bits = [0, 1] * 16 + chips(b"\xaa" * 6) + chips(payload) + [0, 1] * 12
    pay = set(range(HDR + 8 * 66, HDR + 8 * 66 + 17 * 88))
    cov, total_chunks = set(), 0
    mpm, cc = rig.open_devices()
    with mpm, cc:
        for sync in a.sync or ["de25"]:
            word = int(sync, 16)
            w = [(word >> (15 - k)) & 1 for k in range(16)]
            syncs = [q + 16 for q in range(len(bits) - 15) if bits[q:q + 16] == w]
            if not syncs:
                print(f"sync {sync}: not in the stream")
                continue
            rig.run(cc, f"trim {a.trim};preset frskyl;ch {a.ch};seq 255")
            rig.run(mpm, ["frskyl_rx.txt", "stop", f"w 0x04 {word >> 8:#04x}", f"w 0x05 {word & 0xFF:#04x}",
                          f"ch {a.ch}", "s 0x36", "s 0x33", "rx 62"])
            rig.drain(mpm)
            cc.cmd(f"txp 20000 {a.count} {payload.hex()}")
            time.sleep(a.count * 0.02 + 0.3)
            mpm.cmd("stop")
            starts = Counter()
            for e in rig.events(mpm):
                total_chunks += 1
                d = bytes.fromhex(e.split()[3])[:60]
                rb = [(d[i // 8] >> (7 - i % 8)) & 1 for i in range(480)]
                err, q = min((sum(x != y for x, y in zip(rb, bits[q:q + 480])), q) for q in syncs)
                starts[(q, min(err, 99))] += 1
                if err == 0:
                    cov |= set(range(q, min(q + 480, len(bits))))
            print(f"sync {sync}:")
            for (q, err), c in sorted(starts.items()):
                print(f"  stream bit {q - HDR} (payload bit {(q - HDR - 8 * 66) / 11:.0f}): "
                      f"{c} chunks, {err} bit errors")
    print(f"{total_chunks} chunks; payload chips covered exactly: {len(cov & pay)}/{len(pay)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
