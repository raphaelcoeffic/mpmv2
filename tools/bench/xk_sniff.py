#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""An XK transmitter (WLtoys cars, XK X450 / X420) heard by a bench XN297
receiver: the 4-in-1's nRF24L01 (default) or a CC2652 bench (-p v2 / cc).

Bind packets first (address 68 94 a6 d5 c3, channel 7 = XK_bind_hop[0]),
their GID gives the data address and channel as XK_initialize_txid() does;
then one line each time the data payload changes, and the values each byte
took. --gid skips the bind phase (transmitter already bound).

  xk_sniff.py                          switch the transmitter on after the start
  xk_sniff.py --gid f3be8e --secs 30
  xk_sniff.py --addr 32b2b449d3 --ch 55   data address / channel given
"""

from __future__ import annotations

import argparse
import sys
import time

import rig
from mpm_bench import Bench, find_port
from proto_compare import XK_HOP, XK_TX_ADDR, xk_ok

BIND_ADDR = "6894a6d5c3"
BIND_CH = 7


def xk_data(gid: bytes) -> tuple[str, int]:
    """Data address and hopping_frequency[8] of XK_initialize_txid()"""
    s = sum(gid) & 0xFF
    start = (s & 0x1F) + ((s >> 5) & 3)
    return XK_TX_ADDR[start:start + 5].hex(), XK_HOP[(s & 7) << 2]


def stick(b: int) -> str:
    """XK_convert_channel() >> 2: 00 centre, 01..7f one side, 80..ff the other"""
    return "   0" if b == 0 else f"{-(b & 0x7F) if b & 0x80 else b:+4}"


def fields(p: bytes) -> str:
    """XK_send_packet(): T (unsigned), R, E, A, 3 trims, GID, flags, low bits, flags"""
    return (f"T {p[0]:3} R {stick(p[1])} E {stick(p[2])} A {stick(p[3])}  trim {p[4:7].hex(' ')}"
            f"  gid {p[7:10].hex()}  f10 {p[10]:02x} f11 {p[11]:02x}  lo {p[12]:02x} {p[13]:02x}  f14 {p[14]:02x}")


def listen(rx: Bench, setup: str, secs: float):
    """Yields (time us, payload) of the packets with a good checksum for secs"""
    rig.run(rx, setup)
    rig.drain(rx)
    rx.cmd("rx 16")
    t_end = time.time() + secs
    try:
        while time.time() < t_end:
            for e in rig.events(rx):
                f = e.split()
                if len(f) < 4 or "crc=bad" in e:
                    continue
                p = bytes.fromhex(f[3])
                if xk_ok(p):
                    yield int(f[1]), p
            time.sleep(0.01)
    finally:
        rx.cmd("stop")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", default="mpm", help="receiver: mpm (4-in-1, default), v2, cc or a device")
    ap.add_argument("--gid", type=bytes.fromhex, help="transmitter GID (skips the bind phase)")
    ap.add_argument("--addr", help="data address (default: from the GID)")
    ap.add_argument("--ch", type=int, help="data channel (default: from the GID)")
    ap.add_argument("--rate250k", action="store_true", help="X450")
    ap.add_argument("--bind-wait", type=float, default=60.0, help="s")
    ap.add_argument("--secs", type=float, default=60.0, help="data phase, s (Ctrl-C ends it)")
    a = ap.parse_args()

    cc = a.port in ("v2", "cc")
    with (Bench(find_port(a.port), 921600) if cc else Bench(find_port(a.port))) as rx:
        rx.cmd("stop")
        if cc:
            rig.run(rx, "chip cc2652")
        xn = f"xn 1 1 {int(a.rate250k)}"
        gid = a.gid
        if gid is None and a.addr is None:
            print(f"bind phase: address {BIND_ADDR}, channel {BIND_CH}, up to {a.bind_wait:.0f} s", flush=True)
            n = 0
            for _, p in listen(rx, f"{xn};xaddr {BIND_ADDR};ch {BIND_CH}", a.bind_wait):
                n += 1
                print(f"  bind {p.hex(' ')}", flush=True)
                gid = p[7:10]
                if n >= 4:
                    break
            if gid is None:
                print("no bind packet")
                return 1
        addr, ch = xk_data(gid) if gid is not None else (None, None)
        addr, ch = a.addr or addr, a.ch if a.ch is not None else ch
        print(f"data phase: GID {gid.hex() if gid else '?'}, address {addr}, channel {ch}, {a.secs:.0f} s",
              flush=True)
        seen, last, t0, n = [], None, None, 0
        try:
            for t, p in listen(rx, f"{xn};xaddr {addr};ch {ch}", a.secs):
                n += 1
                t0 = t0 if t0 is not None else t
                seen.append(p)
                if p != last:
                    print(f"{(t - t0) / 1e6:7.2f} s  {p.hex(' ')}  {fields(p)}", flush=True)
                    last = p
        except KeyboardInterrupt:
            pass
    print(f"\n{n} data packets")
    if seen:
        print("byte  min max  values")
        for i in range(16):
            v = sorted(set(q[i] for q in seen))
            print(f"  {i:2}   {v[0]:02x}  {v[-1]:02x}  {len(v):3}" +
                  (f"  ({' '.join(f'{x:02x}' for x in v)})" if len(v) <= 8 else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
