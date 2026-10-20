#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""A bound FrSky receiver on the servo monitor (servo_mon.py: its supply on
GP11, a servo output or SBUS on GP12; SBUS is read on GP13, tied to GP12 on
the bench), driven by the port (the LaunchPad, radio_sim.py's frames) or by
the 4-in-1 (MPM bench firmware with the port's module id: the receiver takes
either). Bind it first: F/S button or jumper at power up, then
`radio_sim.py drive --proto P --sub S --bind`.

  frsky_receiver.py rate [--time 20]
      CH1-8 at center: outputs (SBUS frames or pulses) each second, the
      period or the SBUS flags, the telemetry link values
  frsky_receiver.py loss [--off 5] [--rounds 3]
      CH1-8 at max, then each round: signal off (port: no input, the MPM
      stops after 70 ms; mpm: stop), what the output does meanwhile (SBUS:
      when the frame lost and failsafe flags come), and signal on with CH1-8
      at min or max: the time to the first output with the new position
  frsky_receiver.py cold [--rounds 5]
      the sender on, the receiver powered up each round: the time to its
      first output (SBUS: the first frame without the failsafe flag)

Common: --sender port|mpm, --proto 64 --sub 2 --option 0, --servo (a servo
output on GP12 instead of SBUS). Measured with an X8R (UNI-RX firmware) and
a V8R4-II (docs/mpm-core.md, Real receivers).
"""

from __future__ import annotations

import argparse
import queue
import statistics
import time
from collections import Counter

from mpm_bench import Bench, find_port
from radio_sim import CH_CENTER, CH_MAX, CH_MIN, Radio, telem_decode
from servo_mon import ServoMon

SBUS_PIN = 13  # tied to GP12 on the bench
SBUS_LOST, SBUS_FAILSAFE = 0x04, 0x08


def channels(v: int) -> list[int]:
    return [v if i < 8 else CH_CENTER for i in range(16)]


class Output:
    """The receiver's output as (host time, CH1 or the pulse width, SBUS
    flags or 0, period us or 0); low(): below center"""

    def __init__(self, mon: ServoMon, servo: bool):
        self.mon, self.servo = mon, servo
        print(mon.in2("off" if servo else "sbus", SBUS_PIN))

    def drain(self) -> list[tuple[float, int, int, float]]:
        if self.servo:
            # widths over 3000 us: the line rising at power up
            return [(t, w, 0, p) for t, w, p in self.mon.drain() if w < 3000]
        return [(t, ch[0], flags, 0) for t, flags, ch in self.mon.drain_sbus()]

    def low(self, value: int) -> bool:
        return value < (1500 if self.servo else 992)

    def close(self):
        self.mon.in2("off")


class Rig:
    """The devices, the sender's signal on and off, its telemetry"""

    def __init__(self, a):
        self.a = a
        self.radio = Radio(find_port("cc"))
        self.mpm = Bench(find_port("mpm"))
        self.mon = ServoMon()
        self.out = Output(self.mon, a.servo)
        self.radio.set(protocol=0)
        self.mpm.cmd("stop")
        self.mpm.cmd(f"id {a.id:#x}")

    def close(self):
        self.radio.set(protocol=0)
        self.mpm.cmd("stop")
        self.out.close()
        for d in (self.mon, self.mpm, self.radio):
            d.close()

    def on(self, v: int) -> float:
        """The signal on with CH1-8 at v: the time it started (mpm: the
        "proto" command, after the channels)"""
        a = self.a
        if a.sender == "port":
            self.radio.set(protocol=a.proto, sub=a.sub, option=a.option & 0xFF, bind=False, channels=channels(v))
            t = time.monotonic()
            self.radio.sending = True
            return t
        for i, c in enumerate(channels(v)):
            self.mpm.cmd(f"chan {i} {c}")
        t = time.monotonic()
        self.mpm.cmd(f"proto {a.proto} {a.sub} 0 {a.option & 0xFF} 0")
        return t

    def off(self):
        if self.a.sender == "port":
            self.radio.sending = False
        else:
            self.mpm.cmd("stop")

    def telem(self) -> dict[str, int]:
        """The link values since the last call (port: the latest of each,
        and the S.Port frame count; mpm: the bench "telem" command)"""
        if self.a.sender == "mpm":
            r = self.mpm.cmd("telem")
            return {k: int(v) for k, v in (f.split("=") for f in r.split()[1:]) if k != "pkt"}
        out: dict[str, int] = {"frames": 0}
        while True:
            try:
                _, ty, d = self.radio.telem.get_nowait()
            except queue.Empty:
                return out
            v = telem_decode(ty, d)
            out["frames"] += bool(v)
            out.update(v)


def cmd_rate(r: Rig):
    r.on(CH_CENTER)
    time.sleep(3)
    r.out.drain()
    r.telem()
    counts, periods, flags = [], [], Counter()
    for _ in range(int(r.a.time)):
        time.sleep(1)
        o = r.out.drain()
        counts.append(len(o))
        periods += [x[3] for x in o if x[3]]
        flags.update(f for x in o for f in (SBUS_LOST, SBUS_FAILSAFE) if x[2] & f)
    tm = r.telem()
    what = "pulses" if r.a.servo else "frames"
    print(f"{r.a.sender}: {what}/s {statistics.mean(counts):.1f}, min {min(counts)}, per second {counts}")
    if periods:
        print(f"period min {min(periods):.1f} median {statistics.median(periods):.1f} max {max(periods):.1f} us")
    if not r.a.servo:
        print(f"frame lost flags {flags[SBUS_LOST]}, failsafe flags {flags[SBUS_FAILSAFE]}")
    print("telemetry " + " ".join(f"{k} {v}" for k, v in tm.items()))


def cmd_loss(r: Rig):
    r.on(CH_MAX)
    time.sleep(3)
    r.out.drain()
    time.sleep(2)
    o = r.out.drain()
    if not o:
        print("no output")
        return
    print(f"{r.a.sender} on: {len(o) / 2:.1f}/s, CH1 {statistics.median(x[1] for x in o):.0f}")
    for k in range(r.a.rounds):
        t_off = time.monotonic()
        r.off()
        time.sleep(r.a.off)
        o = [x for x in r.out.drain() if x[0] > t_off]

        def first(flag: int) -> str:
            t = next((x[0] for x in o if x[2] & flag), None)
            return "none" if t is None else f"{t - t_off:.2f} s"

        values = Counter(x[1] for x in o if x[0] > t_off + 0.1)
        last = max((x[0] for x in o), default=t_off) - t_off
        line = f"off {r.a.off:.0f} s: {len(o)} outputs, the last {last:.2f} s after off, CH1 {dict(values.most_common(3))}"
        if not r.a.servo:
            line += f"; lost flag after {first(SBUS_LOST)}, failsafe after {first(SBUS_FAILSAFE)}"
        print(line)
        v = CH_MIN if k % 2 == 0 else CH_MAX
        t_on = r.on(v)
        time.sleep(4)
        o = r.out.drain()
        new = next((x for x in o if r.out.low(x[1]) == (v == CH_MIN) and not x[2] & SBUS_FAILSAFE), None)
        if new:
            rate = sum(1 for x in o if x[0] > new[0]) / (time.monotonic() - new[0])
            print(f"on: CH1 {new[1]} {new[0] - t_on:.2f} s after on, {rate:.1f}/s since")
        else:
            print(f"on: no new position in 4 s ({len(o)} outputs)")
    r.off()


def cmd_cold(r: Rig):
    r.on(CH_CENTER)
    time.sleep(2)
    for _ in range(r.a.rounds):
        r.mon.cmd("pwr 0")
        time.sleep(1.5)
        r.out.drain()
        t0 = time.monotonic()
        r.mon.cmd("pwr 1")
        any_out = good = None
        while time.monotonic() - t0 < r.a.wait and good is None:
            for x in r.out.drain():
                any_out = any_out or x[0] - t0
                if not x[2] & SBUS_FAILSAFE:
                    good = x[0] - t0
                    break
            time.sleep(0.05)
        if good is None:
            print(f"{r.a.sender}: no output in {r.a.wait:.0f} s" + (f" (failsafe frames from {any_out:.2f} s)" if any_out else ""))
        else:
            extra = "" if r.a.servo or any_out == good else f" (failsafe frames from {any_out:.2f} s)"
            print(f"{r.a.sender}: first output {good:.2f} s after power on{extra}", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["rate", "loss", "cold"])
    ap.add_argument("--sender", choices=["port", "mpm"], default="port")
    ap.add_argument("--proto", type=int, default=64, help="3 FrSky D, 25 FrSky V, 15 FrSky X, 64 FrSky X2")
    ap.add_argument("--sub", type=int, default=2)
    ap.add_argument("--option", type=int, default=0, help="frequency fine tune")
    ap.add_argument("--servo", action="store_true", help="a servo output on GP12, not SBUS")
    ap.add_argument("--time", type=float, default=20, help="rate: seconds")
    ap.add_argument("--off", type=float, default=5, help="loss: seconds without signal")
    ap.add_argument("--rounds", type=int, default=3, help="loss, cold: rounds")
    ap.add_argument("--wait", type=float, default=60, help="cold: seconds to wait for an output")
    ap.add_argument("--id", type=lambda s: int(s, 0), default=0x1CA156B8,
                    help="the LaunchPad's module id (IEEE address, last 32 bits)")
    a = ap.parse_args()
    r = Rig(a)
    try:
        {"rate": cmd_rate, "loss": cmd_loss, "cold": cmd_cold}[a.command](r)
    finally:
        r.close()


if __name__ == "__main__":
    main()
