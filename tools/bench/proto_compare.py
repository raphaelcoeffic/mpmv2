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
from typing import NamedTuple

import rig
from mpm_bench import Bench, find_port
from radio_sim import Radio

# Channel values sent to both: sticks off center, CH5 and CH7 on
CHANNELS = [1300, 700, 400, 1600, 1800, 204, 1800, 204] + [1024] * 8


def rx_tx_addr(module_id: int, rx_num: int) -> bytes:
    i = (module_id + rx_num) & 0xFFFFFFFF
    a = i.to_bytes(4, "big")
    return a + bytes([(a[2] & 0xF0) | (a[3] & 0x0F)])


class Capture(NamedTuple):
    name: str
    setup: str           # receiver (MPM v2 bench) setup
    rx_len: int
    bind: bool = False   # started with the bind flag
    mask: tuple = ()     # payload bytes left out of the comparison (counters, CRCs)
    valid: object = None  # payload check (protocol CRC without the chip's)


def bayang_capture(addr: bytes, sub: int) -> list[Capture]:
    """Bind channel/address, then a data channel."""
    bind_ch = 10 if sub in (2, 3) else 0  # X16_AH, IRDRONE
    data_ch = (addr[3] & 0x1F) + 0x10  # hopping_frequency[1]
    xn = "preset nrf1m;xn 1 1 0"
    return [Capture("bind", f"{xn};xaddr 0000000000;ch {bind_ch}", 15),
            Capture("data", f"{xn};xaddr {addr.hex()};ch {data_ch}", 15)]


def frskyd_capture(addr: bytes, sub: int) -> list[Capture]:
    """Bind and data on CHANNR 0 (hopping_frequency[0] = 0 for every ID).
    Data: frame and telemetry counters left out."""
    return [Capture("bind", "preset frskyd;ch 0", 20, bind=True),
            Capture("data", "preset frskyd;ch 0", 20, mask=(3, 4))]


def frskyv_capture(addr: bytes, sub: int) -> list[Capture]:
    """Bind on CHANNR 0, data on CHANNR 6 (channel 0 of the seed sequence):
    seed and CRC left out."""
    return [Capture("bind", "preset frskyv;ch 0", 16, bind=True),
            Capture("data", "preset frskyv;ch 6", 16, mask=(3, 4, 14))]


def frskyx_crc_ok(p: bytes) -> bool:
    """FrSkyX_crc() over packet[3..len-2], in the last 2 bytes"""
    crc = 0
    for b in p[3:-2]:
        v = (crc >> 8) ^ b
        crc = ((crc << 8) ^ FRSKYX_CRC_SHORT[v & 15] ^ (0x1081 * (v >> 4))) & 0xFFFF
    return len(p) > 5 and p[-2:] == crc.to_bytes(2, "big")


FRSKYX_CRC_SHORT = [0x0000, 0x1189, 0x2312, 0x329B, 0x4624, 0x57AD, 0x6536, 0x74BF,
                    0x8C48, 0x9DC1, 0xAF5A, 0xBED3, 0xCA6C, 0xDBE5, 0xE97E, 0xF8F7]


def frskyx_capture(addr: bytes, sub: int) -> list[Capture]:
    """Bind on CHANNR 0, data on hopping_frequency[0] (rx_tx_addr[0] & 7):
    channel skip (random), hop index, failsafe flag, sequence and CRC left
    out. FCC D16 v1 only"""
    n = 0x1D
    return [Capture("bind", "preset frskyx;ch 0", n + 1, bind=True, valid=frskyx_crc_ok),
            Capture("data", f"preset frskyx;ch {addr[0] & 7}", n + 1,
                    mask=(4, 5, 7, 21, 22, n - 1, n), valid=frskyx_crc_ok)]


def afhds2a_hop0(module_id: int, addr: bytes) -> int:
    """hopping_frequency[0] of AFHDS2A_calc_channels()"""
    rnd = module_id
    band_no = addr[3] & 3  # idx 0
    rnd = (rnd * 0x0019660D + 0x3C6EF35F) & 0xFFFFFFFF
    return band_no * 41 + 1 + (rnd % 41)


def afhds2a_capture(addr: bytes, sub: int, module_id: int = 0) -> list[Capture]:
    """Bind on A7105 channel 0x0D, data on hopping_frequency[0]: the RX ID
    (EEPROM: from earlier binds on the MPM) left out"""
    rx_id = (5, 6, 7, 8)
    return [Capture("bind", "preset afhds2a;ch 13", 38, bind=True, mask=rx_id),
            Capture("data", f"preset afhds2a;ch {afhds2a_hop0(module_id, addr)}", 38, mask=rx_id)]


# name: (protocol number, captures(addr, sub))
CASES = {"bayang": (14, bayang_capture), "frskyd": (3, frskyd_capture),
         "frskyv": (25, frskyv_capture), "frskyx": (15, frskyx_capture),
         "afhds2a": (28, afhds2a_capture)}


def masked(p: str, mask: tuple) -> str:
    b = bytearray.fromhex(p)
    for i in mask:
        if i < len(b):
            b[i] = 0
    return b.hex()


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

        kw = {"module_id": (a.id + a.rx_num) & 0xFFFFFFFF} if a.case == "afhds2a" else {}
        for c in captures(addr, a.sub, **kw):
            def mpm_start():
                mpm.cmd(f"proto {proto} {a.sub} {a.rx_num} {a.option} {int(c.bind)}")

            def port_start():
                radio.set(protocol=proto, sub=a.sub, rx_num=a.rx_num, option=a.option, bind=c.bind)

            got = {}
            for src, start, stop in (("MPM", mpm_start, lambda: mpm.cmd("stop")),
                                     ("port", port_start, lambda: radio.set(protocol=0, bind=False))):
                time.sleep(0.2)
                got[src] = capture(rx, c.setup, c.rx_len, start, stop, a.secs)
                if c.valid:
                    got[src] = [(t, p) for t, p in got[src] if c.valid(bytes.fromhex(p))]
                print(f"{c.name:5s} {src:4s}: {summary(got[src])}")
            ref = Counter(masked(p, c.mask) for _, p in got["MPM"])
            port = Counter(masked(p, c.mask) for _, p in got["port"])
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
