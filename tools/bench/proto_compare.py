#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""A protocol ported to the MPM core (src/mpm/upstream) against the MULTI-Module
running the same protocol: same module id, rx_num and channels. The MPM v2
(bench firmware) receives each on one channel and address, in the bind phase
and after; the payloads and packet intervals are compared.

  proto_compare.py bayang
  proto_compare.py bayang --secs 6 --sub 1

Devices: the LaunchPad runs the firmware without BENCH (MPM core, driven by
radio_sim.py's frames over its XDS110 port), the 4-in-1 the MPM bench
firmware (its "id" command), the MPM v2 the mpm-v2 bench firmware.
"""

from __future__ import annotations

import argparse
import statistics
import sys
import time
from collections import Counter

import rig
from mpm_bench import Bench, find_port
from radio_sim import Radio

# Channel values sent to both: sticks off center, CH5 and CH7 on
CHANNELS = [1300, 700, 400, 1600, 1800, 204, 1800, 204] + [1024] * 8


def rx_tx_addr(module_id: int, rx_num: int) -> bytes:
    i = (module_id + rx_num) & 0xFFFFFFFF
    a = i.to_bytes(4, "big")
    return a + bytes([(a[2] & 0xF0) | (a[3] & 0x0F)])


def bayang_capture(addr: bytes, sub: int) -> list[tuple[str, str, int]]:
    """(name, receiver setup, rx length): bind channel/address, then a data channel."""
    bind_ch = 10 if sub in (2, 3) else 0  # X16_AH, IRDRONE
    data_ch = (addr[3] & 0x1F) + 0x10  # hopping_frequency[1]
    xn = "preset nrf1m;xn 1 1 0"
    return [("bind", f"{xn};xaddr 0000000000;ch {bind_ch}", 15),
            ("data", f"{xn};xaddr {addr.hex()};ch {data_ch}", 15)]


# name: (protocol number, captures(addr, sub))
CASES = {"bayang": (14, bayang_capture)}


def capture(rx: Bench, setup: str, rx_len: int, start, stop, secs: float) -> list[tuple[int, str]]:
    rig.run(rx, setup)
    rig.drain(rx)
    rx.cmd(f"rx {rx_len}")
    start()
    time.sleep(secs)
    stop()
    rx.cmd("stop")
    time.sleep(0.1)
    out = []
    for e in rig.events(rx):
        f = e.split()
        if len(f) >= 4 and "crc=bad" not in e:
            out.append((int(f[1]), f[3]))
    return out


def summary(pkts: list[tuple[int, str]]) -> str:
    if not pkts:
        return "no packets"
    gaps = [b[0] - a[0] for a, b in zip(pkts, pkts[1:])]
    med = statistics.median(gaps) if gaps else 0
    return f"{len(pkts)} packets, {len(set(p for _, p in pkts))} distinct, median interval {med} us"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("case", choices=CASES)
    ap.add_argument("--sub", type=int, default=0)
    ap.add_argument("--rx-num", type=int, default=0)
    ap.add_argument("--option", type=int, default=0)
    ap.add_argument("--id", type=lambda s: int(s, 0), default=0x1CA156B8,
                    help="the LaunchPad's module id (IEEE address, last 32 bits)")
    ap.add_argument("--secs", type=float, default=4.0)
    a = ap.parse_args()

    proto, captures = CASES[a.case]
    addr = rx_tx_addr(a.id, a.rx_num)
    fails = 0
    with Radio(find_port("cc")) as radio, Bench(find_port("mpm")) as mpm, \
            Bench(find_port("v2"), 921600) as rx:
        rig.run(rx, "chip cc2652")
        mpm.cmd(f"id {a.id:#x}")
        for i, v in enumerate(CHANNELS):
            mpm.cmd(f"chan {i} {v}")
        radio.set(protocol=0, channels=CHANNELS)  # protocol list: no RF until started

        def mpm_start():
            mpm.cmd(f"proto {proto} {a.sub} {a.rx_num} {a.option}")

        def port_start():
            radio.set(protocol=proto, sub=a.sub, rx_num=a.rx_num, option=a.option)

        for name, setup, rx_len in captures(addr, a.sub):
            got = {}
            for src, start, stop in (("MPM", mpm_start, lambda: mpm.cmd("stop")),
                                     ("port", port_start, lambda: radio.set(protocol=0))):
                time.sleep(0.2)
                got[src] = capture(rx, setup, rx_len, start, stop, a.secs)
                print(f"{name:5s} {src:4s}: {summary(got[src])}")
            ref = Counter(p for _, p in got["MPM"])
            port = Counter(p for _, p in got["port"])
            for p, n in ref.most_common(3):
                print(f"      {p}  MPM {n}, port {port.get(p, 0)}")
            only = [p for p in port if p not in ref]
            for p in only[:3]:
                print(f"      {p}  port only ({port[p]})")
            ok = bool(ref) and set(port) == set(ref)
            print(f"      {'same payloads' if ok else 'DIFFERENT'}")
            fails += not ok
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
