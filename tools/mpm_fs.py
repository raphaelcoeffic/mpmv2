#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""Files of the MPM v2's file system (littlefs on the external flash) over
USB-C, while the MPM core runs: the protocols keep running (src/file_server.h).

  mpm_fs.py info                   file system size and use
  mpm_fs.py ls [DIR] [-r]          entries: size, name (dirs end with /)
  mpm_fs.py get PATH [LOCAL]       to LOCAL (default: the name), - for stdout
  mpm_fs.py put LOCAL [PATH]       PATH (default: the name) replaced at once
                                   once the whole file and its CRC-32 check
  mpm_fs.py rm PATH | mv OLD NEW | mkdir PATH
                                   ppm.txt and debug.txt (settings, see
                                   docs/mpm-core.md) are applied at once:
                                   put, rm and mv print the module's errors
  mpm_fs.py eeprom [FILE] [--all]  the EEPROM decoded (mpm_eeprom.py), from the
                                   module or from FILE (a copy got earlier)
  mpm_fs.py log [-t SECONDS]       the module's debug log (src/log.h): the
                                   lines it kept (boot included), then live
                                   until Ctrl-C; debug.txt ("usb = on") turns
                                   it on from power up, no tool needed

Port: --port, else $V2_PORT, else the first CH343. Requests: "FS", seq, op,
payload, CRC-32 at 100000 baud 8E2 (the MULTI link); replies in telemetry
frames of type 0x40. Lost replies: the request again, same seq (the module
answers it from its cache).
"""

import argparse
import os
import struct
import sys
import time
import zlib

import serial
from serial.tools import list_ports

import mpm_eeprom

CH343 = (0x1A86, 0x55D3)
TELEM_FILE = 0x40
TELEM_LOG = 0x41
INFO, LIST, STAT, READ, OPEN, WRITE, CLOSE, REMOVE, RENAME, MKDIR, LOG = range(11)
DATA_MAX = 128
ERRORS = {-2: "no such file or directory", -5: "I/O error", -9: "no upload open",
          -12: "out of memory", -17: "exists", -20: "not a directory", -21: "is a directory",
          -22: "invalid request", -27: "file too large", -28: "no space left",
          -36: "name too long", -39: "directory not empty", -84: "corrupted (CRC or size)"}


class FsError(Exception):
    def __init__(self, status: int):
        super().__init__(f"{ERRORS.get(status, 'error')} ({status})")
        self.status = status


def find_port() -> str:
    env = os.environ.get("V2_PORT")
    if env:
        return env
    ports = sorted(p.device for p in list_ports.comports() if (p.vid, p.pid) == CH343)
    if not ports:
        sys.exit("No MPM v2 (CH343) found, set $V2_PORT or use --port")
    return ports[0]


class Fs:
    def __init__(self, port: str, timeout: float = 1.0, retries: int = 5):
        # DTR and RTS set: the CC2652 runs (CH343: DTR resets it, RTS is BOOT)
        self.ser = serial.Serial(port, 100000, parity=serial.PARITY_EVEN,
                                 stopbits=serial.STOPBITS_TWO, timeout=0.02)
        self.ser.dtr = self.ser.rts = True
        self.timeout, self.retries = timeout, retries
        self.seq = int(time.monotonic() * 1000) & 0xFF
        self.buf = b""
        self.resent = 0
        self.on_log = None  # log text seen while waiting for a reply

    def close(self):
        self.ser.close()

    def frames(self):
        """Telemetry frames read so far: (type, data)"""
        self.buf += self.ser.read(max(1, self.ser.in_waiting))
        while True:
            i = self.buf.find(b"MP")
            if i < 0:
                self.buf = self.buf[-1:]
                return
            self.buf = self.buf[i:]
            if len(self.buf) < 4 or len(self.buf) < 4 + self.buf[3]:
                return
            t, n = self.buf[2], self.buf[3]
            frame, self.buf = self.buf[4:4 + n], self.buf[4 + n:]
            yield t, frame

    def _reply(self, seq: int, op: int, end: float) -> bytes | None:
        while time.monotonic() < end:
            for t, frame in self.frames():
                if t == TELEM_LOG and self.on_log:
                    self.on_log(frame)
                if t != TELEM_FILE or len(frame) < 7:
                    continue
                if zlib.crc32(frame[:-4]) != struct.unpack("<I", frame[-4:])[0]:
                    continue
                if frame[0] == seq and frame[1] == op:
                    return frame[2:-4]
        return None

    def req(self, op: int, payload: bytes = b"") -> bytes:
        self.seq = (self.seq + 1) & 0xFF
        body = bytes([self.seq, op]) + payload
        frame = b"FS" + body + struct.pack("<I", zlib.crc32(body))
        for attempt in range(self.retries):
            if attempt:
                self.resent += 1
            self.ser.write(frame)
            r = self._reply(self.seq, op, time.monotonic() + self.timeout)
            if r is not None:
                status = struct.unpack("b", r[:1])[0]
                if status:
                    raise FsError(status)
                return r[1:]
        raise SystemExit(f"no reply from the module (op {op}, {self.retries} tries)")

    def info(self) -> tuple[int, int, int]:
        return struct.unpack("<III", self.req(INFO))

    def stat(self, path: str) -> tuple[bool, int]:
        r = self.req(STAT, path.encode())
        return r[0] == 2, struct.unpack("<I", r[1:5])[0]

    def ls(self, path: str) -> list[tuple[str, bool, int]]:
        out = []
        while True:
            r = self.req(LIST, struct.pack("<H", len(out)) + path.encode())
            i = 1
            while i < len(r):
                is_dir, size, n = r[i] == 2, struct.unpack("<I", r[i + 1:i + 5])[0], r[i + 5]
                out.append((r[i + 6:i + 6 + n].decode(), is_dir, size))
                i += 6 + n
            if not r[0]:
                return out

    def get(self, path: str) -> bytes:
        data = b""
        while True:
            r = self.req(READ, struct.pack("<IB", len(data), DATA_MAX) + path.encode())
            data += r
            if len(r) < DATA_MAX:
                return data

    def log(self, out, seconds: float | None = None):
        """The log to out until Ctrl-C or seconds: the lines the module kept,
        then live. Back to debug.txt's setting at the end."""
        self.on_log = lambda d: (out.write(d), out.flush())
        self.req(LOG, b"\1")
        end = time.monotonic() + seconds if seconds else None
        try:
            while end is None or time.monotonic() < end:
                for t, frame in self.frames():
                    if t == TELEM_LOG:
                        self.on_log(frame)
        except KeyboardInterrupt:
            pass
        finally:
            self.on_log = None
            self.req(LOG, b"\0")

    def put(self, path: str, data: bytes, progress=None):
        self.req(OPEN, struct.pack("<II", len(data), zlib.crc32(data)) + path.encode())
        for off in range(0, len(data), DATA_MAX):
            self.req(WRITE, struct.pack("<I", off) + data[off:off + DATA_MAX])
            if progress:
                progress(off + len(data[off:off + DATA_MAX]), len(data))
        self.req(CLOSE)


SETTINGS = {"ppm.txt": "ppm.err", "debug.txt": "debug.err"}


def report_settings(fs: Fs, *paths: str):
    """After a change to a settings file: what the module made of it"""
    for path in paths:
        name = path.lstrip("/")
        if name not in SETTINGS:
            continue
        try:
            err = fs.get(SETTINGS[name]).decode(errors="replace")
            print(f"{name}: errors (the defaults for those lines):\n{err}", end="", file=sys.stderr)
        except FsError:
            print(f"{name}: applied", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    p = sub.add_parser("ls")
    p.add_argument("path", nargs="?", default="/")
    p.add_argument("-r", action="store_true", help="recursive")
    p = sub.add_parser("get")
    p.add_argument("path")
    p.add_argument("local", nargs="?")
    p = sub.add_parser("put")
    p.add_argument("local")
    p.add_argument("path", nargs="?")
    p = sub.add_parser("rm")
    p.add_argument("path")
    p = sub.add_parser("mv")
    p.add_argument("old")
    p.add_argument("new")
    p = sub.add_parser("mkdir")
    p.add_argument("path")
    p = sub.add_parser("log")
    p.add_argument("-t", type=float, help="seconds (default: until Ctrl-C)")
    p = sub.add_parser("eeprom")
    p.add_argument("file", nargs="?", help="a local copy instead of the module's")
    p.add_argument("--all", action="store_true", help="the erased entries too")
    a = ap.parse_args()

    if a.cmd == "eeprom" and a.file:
        with open(a.file, "rb") as f:
            print("\n".join(mpm_eeprom.decode(f.read(), a.all)))
        return
    fs = Fs(a.port or find_port())
    try:
        if a.cmd == "info":
            bs, bc, used = fs.info()
            print(f"{bc} blocks of {bs} bytes ({bs * bc // 1024} KB), {used} used ({used * bs // 1024} KB)")
        elif a.cmd == "ls":
            def walk(d: str):
                for name, is_dir, size in sorted(fs.ls(d)):
                    full = f"{d.rstrip('/')}/{name}" if d not in ("", "/") else name
                    print(f"{'' if is_dir else size:>8}  {full}{'/' if is_dir else ''}")
                    if is_dir and a.r:
                        walk(full)
            walk(a.path)
        elif a.cmd == "get":
            t = time.monotonic()
            data = fs.get(a.path)
            if a.local == "-":
                sys.stdout.buffer.write(data)
            else:
                local = a.local or os.path.basename(a.path)
                with open(local, "wb") as f:
                    f.write(data)
                dt = time.monotonic() - t
                print(f"{a.path} -> {local}: {len(data)} bytes in {dt:.1f} s", file=sys.stderr)
        elif a.cmd == "put":
            with open(a.local, "rb") as f:
                data = f.read()
            path = a.path or os.path.basename(a.local)
            t = time.monotonic()

            def progress(done, total):
                print(f"\r{done}/{total} bytes", end="", file=sys.stderr)
            fs.put(path, data, progress if sys.stderr.isatty() else None)
            dt = time.monotonic() - t
            print(f"\r{a.local} -> {path}: {len(data)} bytes in {dt:.1f} s ({len(data) / dt / 1024:.1f} KB/s)",
                  file=sys.stderr)
            report_settings(fs, path)
        elif a.cmd == "rm":
            fs.req(REMOVE, a.path.encode())
            if a.path.lstrip("/") in SETTINGS:
                print(f"{a.path.lstrip('/')}: removed, the defaults", file=sys.stderr)
        elif a.cmd == "mv":
            fs.req(RENAME, a.old.encode() + b"\0" + a.new.encode())
            report_settings(fs, a.new)
        elif a.cmd == "mkdir":
            fs.req(MKDIR, a.path.encode())
        elif a.cmd == "log":
            try:
                fs.log(sys.stdout.buffer, a.t)
            except BrokenPipeError:
                # stdout closed (| head): no more output, not even at exit
                os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
        elif a.cmd == "eeprom":
            print("\n".join(mpm_eeprom.decode(fs.get("eeprom"), a.all)))
        if fs.resent:
            print(f"({fs.resent} requests sent again)", file=sys.stderr)
    except FsError as e:
        sys.exit(f"{a.cmd}: {e}")
    finally:
        fs.close()


if __name__ == "__main__":
    main()
