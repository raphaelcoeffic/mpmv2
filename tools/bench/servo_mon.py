#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""A real receiver's servo output, measured by an RP2040-Zero running
rp2040/main.py (MicroPython): pulse width and period on GP12, the receiver
supply switched by GP11, a second input on GP9 (another servo output or
SBUS). PORT: $SERVO_PORT, else the first Raspberry Pi USB serial (2e8a).

  servo_mon.py install            copy rp2040/main.py to the board (mpremote)
  servo_mon.py pwr 0|1            receiver supply
  servo_mon.py watch [--time S] [--in2 servo|sbus] [--in2-pin N]
                                  per second: pulses, width min/median/max,
                                  period min/median/max, gaps (no pulse for
                                  more than 3 periods); GP9 the same (servo)
                                  or SBUS frames, CH1-4, frame lost and
                                  failsafe flags (--in2-pin 13: SBUS on
                                  GP12's wire, GP13 being tied to it)
"""

from __future__ import annotations

import argparse
import os
import queue
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

import serial
from serial.tools import list_ports


def find_port() -> str:
    if os.environ.get("SERVO_PORT"):
        return os.environ["SERVO_PORT"]
    for p in list_ports.comports():
        if p.vid == 0x2E8A:
            return p.device
    sys.exit("no RP2040 serial port")


class ServoMon:
    """Pulses (host time, width us, period us) in .pulses, a queue; GP9's
    in .pulses2 (servo) or SBUS frames (host time, flags, 16 channels) in
    .sbus (in2("servo"|"sbus"))."""

    def __init__(self, port: str | None = None):
        self.ser = serial.Serial(port or find_port(), 115200, timeout=0.1)
        self.pulses: queue.Queue = queue.Queue()
        self.pulses2: queue.Queue = queue.Queue()
        self.sbus: queue.Queue = queue.Queue()
        self.replies: queue.Queue = queue.Queue()
        self.overruns = 0
        self.sbus_skipped = 0  # bytes skipped to find an SBUS frame
        self._stop = False
        self._thread = threading.Thread(target=self._rx_loop, daemon=True)
        self._thread.start()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        self._stop = True
        self._thread.join(1)
        self.ser.close()

    def _rx_loop(self):
        buf = b""
        while not self._stop:
            buf += self.ser.read(4096)
            *lines, buf = buf.split(b"\n")
            now = time.monotonic()
            for raw in lines:
                a = raw.decode(errors="replace").split()
                if len(a) == 3 and a[0] in "pq":
                    q = self.pulses if a[0] == "p" else self.pulses2
                    q.put((now, int(a[1]) / 10, int(a[2]) / 10))
                elif len(a) == 18 and a[0] == "s":
                    self.sbus.put((now, int(a[1]), [int(x) for x in a[2:]]))
                elif len(a) == 2 and a[0] == "x":
                    self.sbus_skipped += int(a[1])
                elif len(a) == 2 and a[0] == "o":
                    self.overruns += 1
                elif a:
                    self.replies.put(" ".join(a))

    def cmd(self, s: str, timeout: float = 1.0) -> str:
        while not self.replies.empty():
            self.replies.get_nowait()
        self.ser.write(s.encode() + b"\n")
        try:
            return self.replies.get(timeout=timeout)
        except queue.Empty:
            return ""

    def in2(self, mode: str, pin: int = 9) -> str:
        """The second input: off, servo or sbus"""
        return self.cmd(f"in2 {mode} {pin}")

    @staticmethod
    def _drain(q: queue.Queue) -> list:
        out = []
        while not q.empty():
            out.append(q.get_nowait())
        return out

    def drain(self) -> list[tuple[float, float, float]]:
        return self._drain(self.pulses)

    def drain2(self) -> list[tuple[float, float, float]]:
        return self._drain(self.pulses2)

    def drain_sbus(self) -> list[tuple[float, int, list[int]]]:
        return self._drain(self.sbus)


def summary(pulses: list[tuple[float, float, float]]) -> str:
    if not pulses:
        return "no pulses"
    w = [p[1] for p in pulses]
    per = [p[2] for p in pulses]
    med = statistics.median(per)
    gaps = sum(1 for p in per if p > 3 * med)
    return (f"{len(pulses):4d} pulses  width {min(w):7.1f} {statistics.median(w):7.1f} {max(w):7.1f} us"
            f"  period {min(per):7.1f} {med:7.1f} {max(per):7.1f} us  gaps {gaps}")


def sbus_summary(frames: list[tuple[float, int, list[int]]]) -> str:
    """Frames, the last frame's CH1-4, how many frames had the frame lost
    (flags bit 2) and failsafe (bit 3) flags"""
    if not frames:
        return "no SBUS frames"
    lost = sum(1 for f in frames if f[1] & 0x04)
    fs = sum(1 for f in frames if f[1] & 0x08)
    ch = " ".join(f"{c:4d}" for c in frames[-1][2][:4])
    return f"{len(frames):4d} frames  CH1-4 {ch}  lost {lost}  failsafe {fs}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["install", "pwr", "watch"])
    ap.add_argument("value", nargs="?")
    ap.add_argument("-p", "--port")
    ap.add_argument("--time", type=float, default=0, help="watch: seconds (0: Ctrl-C)")
    ap.add_argument("--in2", choices=["servo", "sbus"], help="watch: the second input too")
    ap.add_argument("--in2-pin", type=int, default=9, help="watch: the second input's pin")
    a = ap.parse_args()
    if a.command == "install":
        main_py = Path(__file__).parent / "rp2040" / "main.py"
        return subprocess.call(["mpremote", "connect", a.port or find_port(), "cp", str(main_py), ":main.py", "+", "reset"])
    with ServoMon(a.port) as m:
        if a.command == "pwr":
            print(m.cmd(f"pwr {a.value}"))
            return 0
        print(m.in2(a.in2 or "off", a.in2_pin))
        t0 = time.monotonic()
        try:
            while not a.time or time.monotonic() - t0 < a.time:
                time.sleep(1)
                line = f"{time.monotonic() - t0:6.1f} s {summary(m.drain())}"
                if a.in2 == "servo":
                    line += f" | GP{a.in2_pin} {summary(m.drain2())}"
                elif a.in2 == "sbus":
                    line += f" | {sbus_summary(m.drain_sbus())}  skipped {m.sbus_skipped}"
                print(line, flush=True)
        except KeyboardInterrupt:
            pass
        finally:
            m.in2("off")
    return 0


if __name__ == "__main__":
    sys.exit(main())
