#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""The RX protocols of the MPM core (FrSkyRX, FS2A_RX, BayanRX, DSM_RX)
receiving the 4-in-1: the 4-in-1 (MPM bench firmware, "proto") binds and
then drives with known channels, the port (radio_sim.py's frames) runs the
RX protocol, bound first unless --no-bind, and forwards what it receives
(MULTI telemetry type 13: LQI, RSSI, channels). Pass: once the first
channels came, frames each second and the channels within a tolerance of
the 4-in-1's (CASES).

  rx_test.py frskyx [--sub 0]        FrSky X (D16) -> FrSkyRX
  rx_test.py frskyd                  FrSky D (D8) -> FrSkyRX
  rx_test.py frskyx2                 FrSky X2 -> FrSkyRX
  rx_test.py afhds2a                 AFHDS2A -> FS2A_RX
  rx_test.py dsm [--sub 3]           DSM -> DSM_RX
  rx_test.py bayang                  Bayang -> BayanRX
  rx_test.py frskyx --rx-sub 1       CloneTX: the TX's ID and hops to the
                                     clone entries of the EEPROM (no data)
  ... --save-eeprom FILE             the port's EEPROM at the end (radio_sim.py)

Ports: -p the port's link (default: the LaunchPad, $CC_PORT), the 4-in-1
found as by mpm_bench.py ($MPM_PORT).
"""

from __future__ import annotations

import argparse
import sys
import time

from mpm_bench import Bench, find_port
from radio_sim import Radio, eeprom_get, status_decode

TELEM_RX_CHANNELS = 13
# name: 4-in-1 protocol, port's RX protocol, channels compared, tolerance
# (DSM: its 11 bit scale both ways, up to 9 off; Bayang: CH6 an analog aux,
# then flags)
CASES = {
    "frskyx": (15, 55, 8, 8),
    "frskyd": (3, 55, 8, 8),
    "frskyx2": (64, 55, 8, 8),
    "afhds2a": (28, 56, 8, 8),
    "dsm": (6, 70, 8, 12),
    "bayang": (14, 59, 5, 8),
}
CHANNELS = [300, 700, 1024, 1400, 1800, 500, 1200, 1600] + [1024] * 8


def rx_channels(d: bytes) -> tuple[int, int, list[int | None]]:
    """LQI, RSSI, channels of a type 13 frame (telemetry.c receiver_channels_frame())"""
    lqi, rssi, start, count = d[0], d[1], d[2], d[3]
    bits = int.from_bytes(d[4:], "little")
    return lqi, rssi, [None] * start + [(bits >> (11 * i)) & 0x7FF for i in range(count)]


def bind(r: Radio, mpm: Bench, tx: int, rx: int, a) -> bool:
    """Both in bind until the port's RX protocol ends it"""
    r.set(protocol=rx, sub=a.rx_sub, rx_num=a.rx_num, option=a.option, bind=True)
    time.sleep(0.5)
    mpm.cmd(f"proto {tx} {a.sub} {a.rx_num} 0 1")
    t0 = time.monotonic()
    seen = False
    while time.monotonic() - t0 < a.bind_secs:
        s = r.wait(0x01, 1.0)
        if not s:
            continue
        binding = "binding" in status_decode(s[1])["flags"]
        seen |= binding
        if seen and not binding:
            print(f"bound in {time.monotonic() - t0:.1f} s")
            r.set(bind=False)
            return True
    print(f"not bound after {a.bind_secs:.0f} s (binding flag seen: {seen})")
    r.set(bind=False)
    return False


def data(r: Radio, mpm: Bench, tx: int, rx: int, n_ch: int, tol: int, a) -> bool:
    """The 4-in-1 out of bind, the port's frames counted and checked each second"""
    mpm.cmd(f"proto {tx} {a.sub} {a.rx_num} 0 0")
    r.set(protocol=rx, sub=a.rx_sub, rx_num=a.rx_num, option=a.option, bind=False)
    r.drain()
    t0 = time.monotonic()
    lock = None  # seconds to the first channels (DSM2: its channel scan)
    ok_secs = 0
    bad = 0
    t_end = t0 + a.secs
    while time.monotonic() < t_end:
        t1 = time.monotonic() + 1.0
        frames, last = 0, None
        while (left := t1 - time.monotonic()) > 0:
            got = r.wait(TELEM_RX_CHANNELS, left)
            if got:
                frames += 1
                last = rx_channels(got[1])
        if not last:
            print("  no channels")
            bad += lock is not None
            continue
        if lock is None:
            lock = time.monotonic() - t0
        lqi, rssi, ch = last
        n = min(len(ch), 8)
        off = [i + 1 for i in range(min(n, n_ch)) if ch[i] is not None and abs(ch[i] - CHANNELS[i]) > tol]
        bad += bool(off)
        ok_secs += not off
        print(f"  {frames:3d} frames/s  LQI {lqi:3d}  RSSI {rssi:3d}  ch {ch[:n]}"
              + (f"  off: CH{off}" if off else ""))
    if lock is None:
        print("no channels at all")
        return False
    print(f"channels after {lock:.1f} s, then {ok_secs} s right, {bad} s off or without")
    return bad == 0 and ok_secs >= a.secs / 2


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("case", choices=CASES)
    ap.add_argument("-p", "--port", default="cc", help="the port's link (default: the LaunchPad)")
    ap.add_argument("--sub", type=int, default=0, help="the 4-in-1's sub protocol")
    ap.add_argument("--rx-sub", type=int, default=0, help="the RX protocol's sub protocol (1: CloneTX)")
    ap.add_argument("--rx-num", type=int, default=0)
    ap.add_argument("--option", type=int, default=0, help="the RX protocol's option (FrSkyRX: fine tune, 0 the bind's)")
    ap.add_argument("--no-bind", action="store_true", help="the binding of an earlier run (the port's EEPROM)")
    ap.add_argument("--bind-secs", type=float, default=30.0)
    ap.add_argument("--secs", type=int, default=8, help="seconds of data")
    ap.add_argument("--save-eeprom", metavar="FILE", help="the port's EEPROM into FILE at the end")
    a = ap.parse_args()
    tx, rx, n_ch, tol = CASES[a.case]
    port = find_port(a.port)

    ok = True
    with Bench(find_port("mpm")) as mpm:
        for i, v in enumerate(CHANNELS):
            mpm.cmd(f"chan {i} {v}")
        try:
            with Radio(port) as r:
                r.set(protocol=0)  # protocol list: no RF until started
                time.sleep(0.3)
                if not a.no_bind:
                    ok = bind(r, mpm, tx, rx, a)
                if ok and a.rx_sub != 1:
                    ok = data(r, mpm, tx, rx, n_ch, tol, a)
        finally:
            mpm.cmd("stop")
            if a.save_eeprom:
                eeprom_get(port, a.save_eeprom)
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
