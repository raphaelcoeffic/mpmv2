#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""Host side of the bench mode of the MULTI-Module (Multiprotocol/_Bench.ino,
branch mpm-bench) and of mpm-v2 (src/bench.c), see docs/bench.md.

Any device speaking the same line protocol can be driven by this tool, so the
same commands work against a 4-in-1 module and a CC2652 based module.
PORT can be a device or "mpm" / "cc" (the default is "mpm"): $MPM_PORT / $CC_PORT,
else the first STM32duino USB serial / the first XDS110 port (LaunchPad UART).

  mpm_bench.py ports                         list candidate serial ports
  mpm_bench.py [-p PORT] shell               interactive console
  mpm_bench.py [-p PORT] run FILE            run a command file ('#' comments)
  mpm_bench.py [-p PORT] exec CMD [CMD...]   run commands, e.g. exec "preset frskyd" "ch 10" "rx 20"
  mpm_bench.py link --tx PORT --rx PORT --tx-setup FILE --rx-setup FILE --payload HEX
                                             packet loss test between two devices
"""

from __future__ import annotations

import argparse
import os
import queue
import sys
import threading
import time
from dataclasses import dataclass, field

import serial
from serial.tools import list_ports

MAPLE_VID = 0x1EAF  # STM32duino bootloader and USB serial
XDS110 = (0x0451, 0xBEF3)  # LaunchPad debugger, its first port is the UART


class BenchError(Exception):
    pass


@dataclass
class RxPacket:
    time_us: int
    channel: int
    data: bytes
    crc_ok: bool | None = None
    flags: dict[str, str] = field(default_factory=dict)

    @staticmethod
    def parse(line: str) -> RxPacket:
        # @rx <us> <ch> <hex> [crc=ok|bad] [rpd=0|1]
        parts = line.split()
        flags = dict(p.split("=", 1) for p in parts[4:] if "=" in p)
        crc = flags.get("crc")
        return RxPacket(int(parts[1]), int(parts[2]), bytes.fromhex(parts[3]),
                        None if crc is None else crc == "ok", flags)


class Bench:
    """One bench device. Responses (@ok/@err) and events (@rx/@ev) are split by a reader thread."""

    def __init__(self, port: str, baudrate: int = 921600, echo_debug: bool = False):
        self.ser = serial.Serial(port, baudrate, timeout=0.1)
        self.name = port
        self.echo_debug = echo_debug
        self.responses: queue.Queue[str] = queue.Queue()
        self.events: queue.Queue[str] = queue.Queue()
        self._stop = False
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def _read_loop(self):
        buf = b""
        while not self._stop:
            try:
                buf += self.ser.read(256)
            except serial.SerialException:
                break
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode(errors="replace").strip()
                if line.startswith(("@ok", "@err")):
                    self.responses.put(line)
                elif line.startswith("@"):
                    self.events.put(line)
                elif line and self.echo_debug:
                    print(f"[{self.name}] {line}", file=sys.stderr)

    def close(self):
        self._stop = True
        self._reader.join(timeout=1)
        self.ser.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def cmd(self, line: str, timeout: float = 2.0) -> str:
        """Send one command, return the text after '@ok', raise BenchError on '@err'."""
        while not self.responses.empty():
            self.responses.get_nowait()
        self.ser.write(line.encode() + b"\n")
        try:
            resp = self.responses.get(timeout=timeout)
        except queue.Empty:
            raise BenchError(f"{self.name}: no answer to '{line}'") from None
        if resp.startswith("@err"):
            raise BenchError(f"{self.name}: '{line}' -> {resp[5:]}")
        return resp[4:]

    def run_lines(self, lines, verbose: bool = True):
        for line in lines:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            resp = self.cmd(line)
            if verbose:
                print(f"> {line}" + (f"  -> {resp}" if resp else ""))

    def read_regs(self, reg: int, count: int = 1) -> bytes:
        return bytes.fromhex(self.cmd(f"r {reg:#x} {count}"))

    def write_regs(self, reg: int, data: bytes):
        self.cmd(f"w {reg:#x} {data.hex()}")

    def wait_event(self, prefix: str, timeout: float) -> str | None:
        end = time.monotonic() + timeout
        while (left := end - time.monotonic()) > 0:
            try:
                ev = self.events.get(timeout=left)
            except queue.Empty:
                return None
            if ev.startswith(prefix):
                return ev
        return None

    def packets(self, duration: float):
        """Yield the packets received during duration seconds."""
        end = time.monotonic() + duration
        while (left := end - time.monotonic()) > 0:
            try:
                ev = self.events.get(timeout=left)
            except queue.Empty:
                return
            if ev.startswith("@rx"):
                yield RxPacket.parse(ev)


def find_port(kind: str = "mpm") -> str:
    """Serial port of "mpm" (4-in-1 module) or "cc" (CC2652 LaunchPad), any other value is a device."""
    if kind not in ("mpm", "cc"):
        return kind
    env = os.environ.get(f"{kind.upper()}_PORT")
    if env:
        return env
    if kind == "mpm":
        ports = [p.device for p in list_ports.comports() if p.vid == MAPLE_VID]
    else:
        ports = sorted(p.device for p in list_ports.comports() if (p.vid, p.pid) == XDS110)
    if not ports:
        raise SystemExit(f"No {kind} serial port found, set ${kind.upper()}_PORT or use -p")
    return ports[0]


def setup_lines(path: str | None):
    if not path:
        return []
    with open(path) as f:
        return f.readlines()


def link_test(args):
    """Send numbered packets from one device and count them on the other."""
    payload = bytes.fromhex(args.payload)
    with Bench(find_port(args.tx), args.baud, echo_debug=args.debug) as tx, \
            Bench(find_port(args.rx), args.baud, echo_debug=args.debug) as rx:
        rx.run_lines(setup_lines(args.rx_setup), verbose=args.verbose)
        tx.run_lines(setup_lines(args.tx_setup), verbose=args.verbose)
        rx.cmd(f"rx {args.rx_len}")
        tx.cmd(f"seq {args.seq_index}")
        tx.cmd(f"txp {args.period} {args.count} {payload.hex()}")
        duration = args.count * args.period / 1e6 + 1.0
        if tx.wait_event("@ev txdone", duration) is None:
            print("warning: TX did not report completion", file=sys.stderr)
        time.sleep(0.2)
        seen, good, crc_bad = set(), 0, 0
        while not rx.events.empty():
            ev = rx.events.get_nowait()
            if not ev.startswith("@rx"):
                continue
            pkt = RxPacket.parse(ev)
            if pkt.crc_ok is False:
                crc_bad += 1
                continue
            good += 1
            idx = args.seq_index + args.rx_offset
            if idx < len(pkt.data):
                seen.add(pkt.data[idx])
            if args.verbose:
                print(f"rx {pkt.data.hex()} {pkt.flags}")
        rx_stats = rx.cmd("stop")
        tx_stats = tx.cmd("stop")
        expected = min(args.count, 256)
        print(f"sent={args.count} good={good} ({100 * good / args.count:.1f}%) "
              f"distinct_seq={len(seen)}/{expected} crc_bad={crc_bad}")
        print(f"tx: {tx_stats}\nrx: {rx_stats}")
        return 0 if len(seen) == expected else 1


def shell(bench: Bench):
    def printer():
        while True:
            ev = bench.events.get()
            print(f"\r{ev}\n> ", end="", flush=True)

    threading.Thread(target=printer, daemon=True).start()
    try:
        import readline  # noqa: F401  (line editing and history)
    except ImportError:
        pass
    while True:
        try:
            line = input("> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return
        if not line:
            continue
        try:
            resp = bench.cmd(line)
            print(f"ok {resp}" if resp else "ok")
        except BenchError as e:
            print(f"error: {e}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", default="mpm", help="serial port, or mpm / cc (default: mpm)")
    ap.add_argument("-b", "--baud", type=int, default=921600,
                    help="baud rate, for UART devices like the mpm-v2 LaunchPad (USB CDC ignores it)")
    ap.add_argument("-d", "--debug", action="store_true", help="show the module debug output")
    sub = ap.add_subparsers(dest="action", required=True)
    sub.add_parser("ports")
    sub.add_parser("shell")
    p = sub.add_parser("run")
    p.add_argument("file")
    p.add_argument("--listen", type=float, default=0, help="print events for N seconds after the file")
    p = sub.add_parser("exec")
    p.add_argument("commands", nargs="+")
    p.add_argument("--listen", type=float, default=0, help="print events for N seconds after the commands")
    p = sub.add_parser("link")
    p.add_argument("--tx", required=True, help="port, or mpm / cc")
    p.add_argument("--rx", required=True, help="port, or mpm / cc")
    p.add_argument("--tx-setup")
    p.add_argument("--rx-setup")
    p.add_argument("--payload", required=True, help="hex payload sent by the TX device")
    p.add_argument("--rx-len", type=int, default=0, help="argument of the 'rx' command")
    p.add_argument("--rx-offset", type=int, default=0,
                   help="offset of the TX payload in the received data (ex: 1 for a CC2500 length byte)")
    p.add_argument("--seq-index", type=int, default=1, help="payload byte replaced by the sequence number")
    p.add_argument("--period", type=int, default=10000, help="TX period in us")
    p.add_argument("--count", type=int, default=200)
    p.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if args.action == "ports":
        for port in list_ports.comports():
            vid = f"{port.vid:04x}:{port.pid:04x}" if port.vid is not None else "-"
            print(f"{port.device}\t{vid}\t{port.description}")
        return 0
    if args.action == "link":
        try:
            return link_test(args)
        except (serial.SerialException, BenchError) as e:
            raise SystemExit(f"error: {e}") from None

    port = find_port(args.port)
    try:
        bench = Bench(port, args.baud, echo_debug=args.debug)
    except serial.SerialException as e:
        raise SystemExit(f"error: {e}") from None
    with bench:
        if args.action == "shell":
            shell(bench)
            return 0
        try:
            lines = setup_lines(args.file) if args.action == "run" else args.commands
            bench.run_lines(lines)
        except BenchError as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
        end = time.monotonic() + args.listen
        while (left := end - time.monotonic()) > 0:
            try:
                print(bench.events.get(timeout=left))
            except queue.Empty:
                break
    return 0


if __name__ == "__main__":
    sys.exit(main())
