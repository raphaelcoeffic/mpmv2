#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""A real DSM receiver on the servo monitor (servo_mon.py: its supply on GP11,
a servo output on GP12; bind plug in: bind mode at each power up), bound and
driven by the port (the LaunchPad, radio_sim.py's frames) or by the 4-in-1
(MPM bench firmware, with the port's module and CYRF ids: the receiver takes
either once bound). The MPM v2 receives the bind responses on channel 13.

  dsm_receiver.py bind [--sender port|mpm] [--sub 4] [--time 20]
      bind (sub 4: Auto, then the receiver's mode and channel count, as
      EdgeTX; else that sub protocol, 7 channels), drive with the channels
      at center: the bind responses, pulses per second and period
  dsm_receiver.py loss [--sender port|mpm] [--off 5] [--rounds 3]
      bind with Auto, drive CH1-7 at max, then each round: signal off (port:
      no input, the MPM stops after 70 ms; mpm: stop), what the output does
      meanwhile, and signal on with CH1-7 at min or max: the time to the
      first pulse with the new width

Measured with a Spektrum AR610C and a Lemon-RX DSMP (docs/mpm-core.md, Real
receivers).
"""

from __future__ import annotations

import argparse
import statistics
import time
from collections import Counter

import rig
from mpm_bench import Bench, find_port
from proto_compare import dsm_cyrf_id
from radio_sim import CH_CENTER, CH_MAX, CH_MIN, Radio, dsm_auto
from servo_mon import ServoMon

BIND_SECS = 8  # the MPM's DSM bind with its response read: about 6 s


def channels(v: int) -> list[int]:
    return [v if i < 7 else CH_CENTER for i in range(16)]


class Rig:
    """The devices, the sender's signal on and off"""

    def __init__(self, a):
        self.a = a
        self.radio = Radio(find_port("cc"))
        self.mpm = Bench(find_port("mpm"))
        self.rx = Bench(find_port("v2"), 921600)
        self.mon = ServoMon()
        self.sub, self.nch = a.sub, 7

    def close(self):
        self.radio.set(protocol=0)
        self.mpm.cmd("stop")
        for d in (self.mon, self.rx, self.mpm, self.radio):
            d.close()

    def bind(self) -> tuple[Counter, int]:
        """Power cycles the receiver into bind and binds it: the bind responses
        received (payload hex: count, those starting with the inverted module
        id) and the number of other packets; Auto: self.sub and self.nch from
        the most frequent response"""
        a, radio, mpm, rx, mon = self.a, self.radio, self.mpm, self.rx, self.mon
        radio.set(protocol=0)
        mpm.cmd("stop")
        mpm.cmd(f"id {a.id:#x}")
        mpm.cmd(f"cyrfid {dsm_cyrf_id(a.id).hex()}")
        prefix = bytes(b ^ 0xFF for b in a.id.to_bytes(4, "big")).hex()
        rig.run(rx, f"chip cc2652;preset dsmbind;cyrf 0 98881be430790384 0 0 0 1;ch 13;prefix {prefix}")
        rig.drain(rx)
        rx.cmd("rx 10")
        mon.cmd("pwr 0")
        time.sleep(1)
        mon.cmd("pwr 1")
        time.sleep(1.5)
        option = 0 if a.sub == 4 else self.nch
        if a.sender == "port":
            radio.set(protocol=6, sub=a.sub, option=option, bind=True, channels=channels(CH_CENTER))
            radio.sending = True
        else:
            for i in range(16):
                mpm.cmd(f"chan {i} {CH_CENTER}")
            mpm.cmd(f"proto 6 {a.sub} 0 {option} 1")
        time.sleep(BIND_SECS)
        rx.cmd("stop")
        time.sleep(0.1)
        packets = [e.split()[3] for e in rig.events(rx) if "crc=bad" not in e]
        resp = Counter(p for p in packets if p.startswith(prefix))
        if a.sub == 4 and resp:
            r = bytes.fromhex(resp.most_common(1)[0][0])
            if len(r) >= 7:
                self.sub, self.nch = dsm_auto(r[6], r[5])
        return resp, len(packets) - sum(resp.values())

    def on(self, v: int):
        if self.a.sender == "port":
            self.radio.set(protocol=6, sub=self.sub, option=self.nch, bind=False, channels=channels(v))
            self.radio.sending = True
        else:
            for i in range(7):
                self.mpm.cmd(f"chan {i} {v}")
            self.mpm.cmd(f"proto 6 {self.sub} 0 {self.nch} 0")

    def off(self):
        if self.a.sender == "port":
            self.radio.sending = False
        else:
            self.mpm.cmd("stop")


def print_bind(r: Rig, bound: tuple[Counter, int]):
    resp, other = bound
    top = ", ".join(f"{p} x{n}" for p, n in resp.most_common(2))
    print(f"{r.a.sender} sub {r.a.sub}: {sum(resp.values())} bind responses ({top or 'none'}),"
          f" {other} other packets -> sub {r.sub}, {r.nch} channels", flush=True)


def cmd_bind(r: Rig):
    print_bind(r, r.bind())
    r.on(CH_CENTER)
    time.sleep(2)
    r.mon.drain()
    counts, periods = [], []
    for _ in range(int(r.a.time)):
        time.sleep(1)
        p = r.mon.drain()
        counts.append(len(p))
        periods += [x[2] for x in p]
    print(f"pulses/s {statistics.mean(counts):.1f}, per second {counts}")
    if periods:
        print(f"period min {min(periods):.1f} median {statistics.median(periods):.1f}"
              f" max {max(periods):.1f} us")


def cmd_loss(r: Rig):
    print_bind(r, r.bind())
    r.on(CH_MAX)
    time.sleep(3)
    p = r.mon.drain()
    if not p:
        print("no pulses")
        return
    print(f"on: {len(p) / 3:.1f} pulses/s, width {statistics.median(x[1] for x in p):.0f} us")
    for k in range(r.a.rounds):
        old = statistics.median(x[1] for x in p[-10:])
        t_off = time.monotonic()
        r.off()
        time.sleep(r.a.off)
        p = r.mon.drain()
        after = [x for x in p if x[0] > t_off + 0.1]
        widths = Counter(round(x[1] / 10) * 10 for x in after)
        last = max((x[0] for x in p), default=t_off) - t_off
        print(f"off {r.a.off:.0f} s: {len(after)} pulses (widths {dict(widths.most_common(3))}),"
              f" the last {last:.2f} s after off")
        t_on = time.monotonic()
        r.on(CH_MIN if k % 2 == 0 else CH_MAX)
        time.sleep(4)
        p = r.mon.drain()
        new = [x for x in p if abs(x[1] - old) > 200]
        if new:
            rate = len(new) / (p[-1][0] - new[0][0] + 0.022)
            print(f"on: width {new[0][1]:.0f} us {new[0][0] - t_on:.2f} s after on,"
                  f" {rate:.1f} pulses/s since")
        else:
            print(f"on: no new width in 4 s ({len(p)} pulses)")
    r.off()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["bind", "loss"])
    ap.add_argument("--sender", choices=["port", "mpm"], default="port")
    ap.add_argument("--sub", type=int, default=4, help="bind: DSM sub protocol (4: Auto)")
    ap.add_argument("--time", type=float, default=20, help="bind: seconds of driving")
    ap.add_argument("--off", type=float, default=5, help="loss: seconds without signal")
    ap.add_argument("--rounds", type=int, default=3, help="loss: off / on rounds")
    ap.add_argument("--id", type=lambda s: int(s, 0), default=0x1CA156B8,
                    help="the LaunchPad's module id (IEEE address, last 32 bits)")
    a = ap.parse_args()
    if a.command == "loss":
        a.sub = 4
    r = Rig(a)
    try:
        (cmd_bind if a.command == "bind" else cmd_loss)(r)
    finally:
        r.close()


if __name__ == "__main__":
    main()
