#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""The module bay simulator (baysim/, an RP2040-Zero) in front of the MPM v2:
GP12 to the bay's PPM pin, GP13 from its S.Port pin, the module supply
through GP11. Two USB serial ports: the link (a bridge to the bay lines,
inverted, at the host's line settings) and the control port.

  baysim.py pwr 0|1                 module supply
  baysim.py level                   pad levels of both lines
  baysim.py flash FILE [--delay 0.2] [--no-verify]
      as EdgeTX: power off 3 s, stk500_flash.py syncing (every 100 ms),
      power on after --delay s; the first frames after power on from the
      baysim trace
  baysim.py window drive|float|frames DELAYS
      the boot loader's window: power on, one STK_GET_SYNC at each delay
      (ms, comma separated), answered or not. drive: the radio line idle;
      float: no radio (CPPM left to the module's pull-up); frames: MULTI
      frames (100k 8E2) from 5 ms, then the sync
  baysim.py reboot [--proto 127] [--sub 0] [--off 1] [--rounds 5]
      a radio rebooting with the model in the air: radio_sim.py's frames
      (100k 8E2, every 7 ms) all along, the module powered off for --off s
      then on: the time to its first telemetry (the status frame of the
      protocol start) and to the receiver's first (S.Port frames, a bound
      FrSky X receiver), from the baysim trace

Results: docs/update.md, "Tested over the module bay".
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import threading
import time
from pathlib import Path

import serial
from serial.tools import list_ports

ROOT = Path(__file__).resolve().parents[2]
VID, PID = 0x2E8A, 0x4D32
SYNC = bytes([0x30, 0x20])  # STK_GET_SYNC, CRC_EOP
FRAME = bytes([0x55, 0x06, 0x00]) + bytes(23)  # a MULTI frame, protocol 6


def find_ports() -> tuple[str, str]:
    """The link and control ports (USB interfaces 0 and 2: their device
    names sort in that order)"""
    ports = sorted(p.device for p in list_ports.comports() if p.vid == VID and p.pid == PID)
    if len(ports) != 2:
        sys.exit(f"baysim not found ({ports})")
    return ports[0], ports[1]


class BaySim:
    """The control port; with trace on, the event lines in .events"""

    def __init__(self, port: str):
        self.ser = serial.Serial(port, 115200, timeout=0.1)
        self.events: list[list[str]] = []
        self._replies: list[str] = []
        self._stop = False
        self._thread = threading.Thread(target=self._rx_loop, daemon=True)
        self._thread.start()

    def _rx_loop(self):
        buf = b""
        while not self._stop:
            buf += self.ser.read(4096)
            *lines, buf = buf.split(b"\n")
            for raw in lines:
                a = raw.decode(errors="replace").split()
                if a and a[0] in ("P", "T", "R"):
                    self.events.append(a)
                elif a:
                    self._replies.append(" ".join(a))

    def cmd(self, s: str, timeout: float = 1.0) -> str:
        self._replies.clear()
        self.ser.write(s.encode() + b"\n")
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if self._replies:
                return self._replies.pop(0)
            time.sleep(0.001)
        return ""

    def close(self):
        self._stop = True
        self._thread.join(1)
        self.ser.close()


def cmd_flash(bay: BaySim, link: str, a):
    bay.cmd("trace 0")
    bay.cmd("tx drive")
    bay.cmd("invert 1")
    bay.cmd("pwr 0")
    time.sleep(3)
    bay.events.clear()
    bay.cmd("trace 1")
    args = ["uv", "run", "-q", str(ROOT / "tools/stk500_flash.py"), "--port", link, "--no-reset", a.file]
    if not a.no_verify:
        args.append("--verify")
    p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(a.delay)
    bay.cmd("pwr 1")
    out = p.communicate(timeout=600)[0]
    time.sleep(0.3)
    bay.cmd("trace 0")
    print("\n".join(l for l in out.splitlines() if "/" not in l or " in " in l))
    t_on = next(int(e[1]) for e in bay.events if e[0] == "P" and e[2] == "1")
    for e in [e for e in bay.events if e[0] != "P"][:8]:
        print(f"{e[0]} {(int(e[1]) - t_on) / 1000:9.3f} ms {' '.join(e[2:])[:72]}")
    return p.returncode


def cmd_window(bay: BaySim, link_port: str, a):
    link = serial.Serial(link_port, 57600, timeout=0.05)
    for d in (int(x) for x in a.delays.split(",")):
        bay.cmd("pwr 0")
        bay.cmd("tx float" if a.mode == "float" else "tx drive")
        link.baudrate = 57600
        time.sleep(1.0)
        t0 = time.monotonic()
        bay.cmd("pwr 1")
        if a.mode == "frames":
            time.sleep(0.005)
            link.baudrate, link.parity, link.stopbits = 100000, serial.PARITY_EVEN, 2
            while time.monotonic() - t0 < d / 1000 - 0.03:
                link.write(FRAME)
                time.sleep(0.007)
            time.sleep(0.01)
            link.baudrate, link.parity, link.stopbits = 57600, serial.PARITY_NONE, 1
        while time.monotonic() - t0 < d / 1000:
            pass
        if a.mode == "float":
            bay.cmd("tx drive")
        link.reset_input_buffer()
        link.write(SYNC)
        r = link.read(16)
        print(f"{a.mode} {d:4d} ms: {r.hex() or '-'}", flush=True)
    bay.cmd("tx drive")
    link.close()


def cmd_reboot(bay: BaySim, link_port: str, a):
    sys.path.insert(0, str(Path(__file__).parent))
    from radio_sim import Radio

    with Radio(link_port) as radio:
        radio.set(protocol=a.proto, sub=a.sub, option=0)
        time.sleep(1)
        for _ in range(a.rounds):
            bay.cmd("pwr 0")
            time.sleep(a.off)
            bay.events.clear()
            bay.cmd("trace 1")
            bay.cmd("pwr 1")
            time.sleep(a.wait)
            bay.cmd("trace 0")
            ev = list(bay.events)
            t_on = next(int(e[1]) for e in ev if e[0] == "P" and e[2] == "1")
            # telemetry frames start with "MP" (4d50); the power up breaks are 00
            first = next((e for e in ev if e[0] == "R" and "4d50" in e[2]), None)
            sent = next((e for e in ev if e[0] == "T" and int(e[1]) > t_on), None)
            rx = next((e for e in ev if e[0] == "R" and "4d5002" in e[2]), None)
            line = f"off {a.off:.1f} s: first frame sent {(int(sent[1]) - t_on) / 1000:.1f} ms after power on, "
            line += f"first telemetry {(int(first[1]) - t_on) / 1000:.1f} ms" if first else "no telemetry"
            if rx:
                line += f", the receiver's {(int(rx[1]) - t_on) / 1000:.1f} ms"
            print(line, flush=True)
        radio.set(protocol=0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)
    p = sub.add_parser("pwr")
    p.add_argument("value", choices=["0", "1"])
    sub.add_parser("level")
    p = sub.add_parser("flash")
    p.add_argument("file", nargs="?", default=str(ROOT / "build-bl/mpm-v2.bin"))
    p.add_argument("--delay", type=float, default=0.2, help="s from the sync start to power on")
    p.add_argument("--no-verify", action="store_true")
    p = sub.add_parser("window")
    p.add_argument("mode", choices=["drive", "float", "frames"])
    p.add_argument("delays", help="ms after power on, comma separated")
    p = sub.add_parser("reboot")
    p.add_argument("--proto", type=int, default=127)
    p.add_argument("--sub", type=int, default=0)
    p.add_argument("--off", type=float, default=1.0, help="s without power")
    p.add_argument("--wait", type=float, default=1.5, help="s of trace after power on")
    p.add_argument("--rounds", type=int, default=5)
    a = ap.parse_args()

    link, ctrl = find_ports()
    bay = BaySim(ctrl)
    try:
        if a.command == "pwr":
            print(bay.cmd(f"pwr {a.value}"))
        elif a.command == "level":
            print(bay.cmd("level"))
        elif a.command == "flash":
            return cmd_flash(bay, link, a)
        elif a.command == "window":
            cmd_window(bay, link, a)
        else:
            cmd_reboot(bay, link, a)
    finally:
        bay.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
