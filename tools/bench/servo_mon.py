#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""A real receiver's servo output, measured by an RP2040-Zero running
rp2040/main.py (MicroPython): pulse width and period on GP12, the receiver
supply switched by GP11. PORT: $SERVO_PORT, else the first Raspberry Pi USB
serial (2e8a).

  servo_mon.py install            copy rp2040/main.py to the board (mpremote)
  servo_mon.py pwr 0|1            receiver supply
  servo_mon.py watch [--time S]   per second: pulses, width min/median/max,
                                  period min/median/max, gaps (no pulse for
                                  more than 3 periods)
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
    """Pulses (host time, width us, period us) in .pulses, a queue."""

    def __init__(self, port: str | None = None):
        self.ser = serial.Serial(port or find_port(), 115200, timeout=0.1)
        self.pulses: queue.Queue = queue.Queue()
        self.replies: queue.Queue = queue.Queue()
        self.overruns = 0
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
                if len(a) == 3 and a[0] == "p":
                    self.pulses.put((now, int(a[1]) / 10, int(a[2]) / 10))
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

    def drain(self) -> list[tuple[float, float, float]]:
        out = []
        while not self.pulses.empty():
            out.append(self.pulses.get_nowait())
        return out


def summary(pulses: list[tuple[float, float, float]]) -> str:
    if not pulses:
        return "no pulses"
    w = [p[1] for p in pulses]
    per = [p[2] for p in pulses]
    med = statistics.median(per)
    gaps = sum(1 for p in per if p > 3 * med)
    return (f"{len(pulses):4d} pulses  width {min(w):7.1f} {statistics.median(w):7.1f} {max(w):7.1f} us"
            f"  period {min(per):7.1f} {med:7.1f} {max(per):7.1f} us  gaps {gaps}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["install", "pwr", "watch"])
    ap.add_argument("value", nargs="?")
    ap.add_argument("-p", "--port")
    ap.add_argument("--time", type=float, default=0, help="watch: seconds (0: Ctrl-C)")
    a = ap.parse_args()
    if a.command == "install":
        main_py = Path(__file__).parent / "rp2040" / "main.py"
        return subprocess.call(["mpremote", "connect", a.port or find_port(), "cp", str(main_py), ":main.py", "+", "reset"])
    with ServoMon(a.port) as m:
        if a.command == "pwr":
            print(m.cmd(f"pwr {a.value}"))
            return 0
        t0 = time.monotonic()
        try:
            while not a.time or time.monotonic() - t0 < a.time:
                time.sleep(1)
                print(f"{time.monotonic() - t0:6.1f} s {summary(m.drain())}", flush=True)
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
