#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""A toy car driven through a known sequence by the MPM core (radio_sim
frames on the port's link, default the MPM v2's USB): bind, steering, throttle,
brake / reverse, signal loss, each step printed with its time.

Car mapping (WLtoys K989 on V2x2, WLtoys 284131 on XK Cars): throttle on CH2
(low = forward), steering on CH4 (high = left); CH3 and CH5..CH16 at minimum
(XK: throttle byte, modes and buttons 00 as the car transmitters send).

  car_drive.py                       XK Cars, bind first (car on, its transmitter off)
  car_drive.py --no-bind             already bound to this module
  car_drive.py --proto 5 --sub 0     V2x2
  car_drive.py --throttle 50
"""

from __future__ import annotations

import argparse
import sys
import time

from mpm_bench import find_port
from radio_sim import Radio, status_decode

CH_MIN, CH_MID, CH_MAX = 204, 1024, 1844


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", default="v2", help="the port's link (default: MPM v2 USB)")
    ap.add_argument("--proto", type=int, default=62)
    ap.add_argument("--sub", type=int, default=2)
    ap.add_argument("--rx-num", type=int, default=0)
    ap.add_argument("--throttle", type=float, default=30.0, help="%% of full throttle")
    ap.add_argument("--no-bind", action="store_true")
    a = ap.parse_args()

    thr = round((CH_MAX - CH_MID) * a.throttle / 100)
    fwd, rev = CH_MID - thr, CH_MID + thr
    steps = [  # name, seconds, CH2 (throttle), CH4 (steering), RF on
        ("neutral", 3, CH_MID, CH_MID, True),
        ("steer left (full)", 2, CH_MID, CH_MAX, True),
        ("centre", 2, CH_MID, CH_MID, True),
        ("steer right (full)", 2, CH_MID, CH_MIN, True),
        ("centre", 2, CH_MID, CH_MID, True),
        ("forward", 2, fwd, CH_MID, True),
        ("neutral", 2, CH_MID, CH_MID, True),
        ("forward", 2, fwd, CH_MID, True),
        ("reverse right after forward (brake?)", 2, rev, CH_MID, True),
        ("neutral", 1, CH_MID, CH_MID, True),
        ("reverse", 2, rev, CH_MID, True),
        ("neutral", 2, CH_MID, CH_MID, True),
        ("forward + left", 2, fwd, CH_MAX, True),
        ("forward + right", 2, fwd, CH_MIN, True),
        ("neutral", 2, CH_MID, CH_MID, True),
        ("forward", 2, fwd, CH_MID, True),
        ("SIGNAL OFF, forward still asked (wheels should stop)", 3, fwd, CH_MID, False),
        ("signal back, forward", 2, fwd, CH_MID, True),
        ("neutral", 2, CH_MID, CH_MID, True),
    ]

    def chans(ch2: int, ch4: int) -> list[int]:
        c = [CH_MIN] * 16
        c[0], c[1], c[3] = CH_MID, ch2, ch4
        return c

    proto = dict(protocol=a.proto, sub=a.sub, rx_num=a.rx_num)
    with Radio(find_port(a.port)) as r:
        t0 = time.monotonic()

        def say(s: str):
            print(f"{time.monotonic() - t0:6.1f} s  {s}", flush=True)

        r.set(**proto, channels=chans(CH_MID, CH_MID), bind=not a.no_bind)
        if not a.no_bind:
            say("bind")
            time.sleep(1.0)
            end = time.monotonic() + 10
            while time.monotonic() < end:
                flags = None
                while not r.telem.empty():
                    _, ty, d = r.telem.get_nowait()
                    if ty == 0x01:
                        flags = status_decode(d)["flags"]
                if flags is not None and "binding" not in flags:
                    break
                time.sleep(0.05)
            r.set(bind=False)
            say("bind done")
        for name, secs, ch2, ch4, rf in steps:
            r.set(**(proto if rf else dict(protocol=0)), channels=chans(ch2, ch4))
            say(f"{name}  (CH2 {ch2}, CH4 {ch4})")
            time.sleep(secs)
        r.set(protocol=0)
        say("end (RF off)")
        time.sleep(0.2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
