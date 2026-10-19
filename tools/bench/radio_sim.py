#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""The radio side of the MULTI-Module serial link, for the mpm-v2 firmware
(MPM core, not the bench): frames every 7 ms at 100000 baud 8E2 as EdgeTX
sends them, telemetry decoded. LaunchPad: XDS110 back channel ($CC_PORT).

  radio_sim.py status [--proto 127] [--sub 0] [--option 40] [--time 2]
  radio_sim.py scan                      protocol list, as EdgeTX reads it
  radio_sim.py bind [--proto 127]        bind flag set for 1 s, then cleared
  radio_sim.py link [--count 500]        test protocol -> 4-in-1 MPM bench (NRF RX)
"""

from __future__ import annotations

import argparse
import queue
import struct
import sys
import threading
import time
from typing import Any

import serial

from mpm_bench import Bench, find_port

FRAME_PERIOD = 0.007
CH_MIN, CH_CENTER, CH_MAX = 204, 1024, 1844
PROTO_TEST = 127
TEST_ADDR = "32764d504d"  # TX_ADDR order: "MPMv2" on air
TEST_LEN = 30

STATUS_FLAGS = ["input", "serial", "valid", "binding", "wait_bind", "failsafe", "ch_map", "buf_low"]


def encode(protocol=0, sub=0, rx_num=0, option=0, channels=None, bind=False, autobind=False,
           range_check=False, low_power=False, failsafe=False, v2=True, disable_telem=False,
           disable_ch_map=False, data=b"") -> bytes:
    """A frame of the radio (EdgeTX pulses/multi.cpp, Multiprotocol.h)."""
    channels = channels or [CH_CENTER] * 16
    b = bytearray(26)
    b[0] = 0x54 | (0x02 if failsafe else 0) | (0 if protocol & 0x20 else 0x01)
    b[1] = (protocol & 0x1F) | (0x80 if bind else 0) | (0x40 if autobind else 0) | (0x20 if range_check else 0)
    b[2] = (rx_num & 0x0F) | (sub & 7) << 4 | (0x80 if low_power else 0)
    b[3] = option & 0xFF
    bits = 0
    for i, c in enumerate(channels):
        bits |= (c & 0x7FF) << (11 * i)
    b[4:26] = bits.to_bytes(22, "little")
    if v2:
        b.append((protocol & 0xC0) | (rx_num & 0x30) | (0x02 if disable_telem else 0) | (0x01 if disable_ch_map else 0))
        b += data
    return bytes(b)


def channels_decode(d: bytes) -> list[int]:
    bits = int.from_bytes(d[:22], "little")
    return [(bits >> (11 * i)) & 0x7FF for i in range(16)]


def status_decode(d: bytes) -> dict:
    flags = [n for i, n in enumerate(STATUS_FLAGS) if d[0] >> i & 1]
    return {"flags": flags, "version": ".".join(str(v) for v in d[1:5]), "order": d[5], "next": d[6],
            "prev": d[7], "name": d[8:15].rstrip(b"\0").decode(errors="replace"),
            "n_sub": d[15] & 0x0F, "option_type": d[15] >> 4,
            "sub": d[16:24].rstrip(b"\0").decode(errors="replace")}


class Radio:
    """Sends the current frame every 7 ms, collects the telemetry frames."""

    def __init__(self, port: str):
        self.ser = serial.Serial(port, 100000, parity=serial.PARITY_EVEN, stopbits=serial.STOPBITS_TWO,
                                 timeout=0.01)
        self.frame: dict[str, Any] = dict(protocol=PROTO_TEST)
        self.telem: queue.Queue = queue.Queue()
        self.sending = True
        self.frames_sent = 0
        self._stop = False
        self._threads = [threading.Thread(target=f, daemon=True) for f in (self._tx_loop, self._rx_loop)]
        for t in self._threads:
            t.start()

    def set(self, **kw):
        self.frame = {**self.frame, **kw}

    def _tx_loop(self):
        next_t = time.monotonic()
        while not self._stop:
            if self.sending:
                self.ser.write(encode(**self.frame))
                self.frames_sent += 1
            next_t += FRAME_PERIOD
            time.sleep(max(0.0, next_t - time.monotonic()))

    def _rx_loop(self):
        buf = bytearray()
        while not self._stop:
            buf += self.ser.read(256)
            while len(buf) >= 4:
                i = buf.find(b"MP")
                if i < 0:
                    del buf[:-1]
                    break
                del buf[:i]
                if len(buf) < 4 or len(buf) < 4 + buf[3]:
                    break
                self.telem.put((time.monotonic(), buf[2], bytes(buf[4:4 + buf[3]])))
                del buf[:4 + buf[3]]

    def wait(self, type_: int, timeout: float = 1.0):
        end = time.monotonic() + timeout
        while (left := end - time.monotonic()) > 0:
            try:
                t, ty, d = self.telem.get(timeout=left)
            except queue.Empty:
                break
            if ty == type_:
                return t, d
        return None

    def drain(self):
        while not self.telem.empty():
            self.telem.get_nowait()

    def close(self):
        self._stop = True
        for t in self._threads:
            t.join()
        self.ser.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def cmd_status(r: Radio, a):
    r.set(protocol=a.proto, sub=a.sub, option=a.option)
    end = time.monotonic() + a.time
    last = None
    while time.monotonic() < end:
        s = r.wait(0x01, end - time.monotonic())
        if not s:
            break
        t, d = s
        st = status_decode(d)
        dt = f"+{(t - last) * 1000:.0f} ms" if last else ""
        last = t
        print(f"status {dt:>9s} {st}")
    return 0 if last else 1


def cmd_scan(r: Radio, a):
    """As EdgeTX (io/multi_protolist.cpp): option 0xFF gives the count, then
    each index until 0xFF comes back."""
    r.set(protocol=0, sub=0, option=0xFF)
    reply = r.wait(0x11, 3.0)
    if not reply:
        print("no protocol list reply")
        return 1
    count = reply[1][0]
    print(f"{count} protocols")
    idx = 0
    while True:
        r.drain()
        r.set(option=idx)
        reply = r.wait(0x11, 0.5)
        if not reply:
            print(f"  [{idx}] no reply")
            return 1
        d = reply[1]
        if d[0] == 0xFF:
            break
        name, rest = d[1:].split(b"\0", 1)
        flags, n_sub = rest[0], rest[1]
        subs = []
        if n_sub:
            ln = rest[2]
            subs = [rest[3 + i * ln:3 + (i + 1) * ln].decode().rstrip() for i in range(n_sub)]
        print(f"  [{idx}] {d[0]:3d} {name.decode():8s} option={flags >> 4} failsafe={flags & 1} "
              f"chmap={flags >> 1 & 1} subs={subs}")
        idx += 1
    return 0 if idx == count else 1


def cmd_bind(r: Radio, a):
    r.set(protocol=a.proto, sub=a.sub, option=a.option)
    time.sleep(0.6)
    r.drain()
    r.set(bind=True)
    t0 = time.monotonic()
    seen_binding = False
    while time.monotonic() - t0 < 1.0:
        s = r.wait(0x01, 0.6)
        if s and "binding" in status_decode(s[1])["flags"]:
            seen_binding = True
    r.set(bind=False)
    time.sleep(0.6)
    r.drain()
    s = r.wait(0x01, 0.6)
    done = s and "binding" not in status_decode(s[1])["flags"]
    print(f"binding flag while bind: {seen_binding}, cleared after: {bool(done)}")
    return 0 if seen_binding and done else 1


def cmd_link(r: Radio, a):
    """The test protocol's packets on the 4-in-1 MPM (NRF24L01): count,
    channels, flags, period (MPM RX time) and the CC2652's callback times."""
    chans = [CH_MIN + (CH_MAX - CH_MIN) * i // 15 for i in range(16)]
    sub_rf = {0: "", 1: "w 0x06 0x22"}[a.sub]
    with Bench(find_port("mpm")) as mpm:
        for c in [f"preset nrf", "w 0x00 0e", f"w 0x0a {TEST_ADDR}", f"w 0x10 {TEST_ADDR}",
                  f"w 0x11 {TEST_LEN:02x}", sub_rf, f"ch {a.option}"]:
            if c:
                mpm.cmd(c)
        r.set(protocol=PROTO_TEST, sub=a.sub, option=a.option, rx_num=3, channels=chans)
        time.sleep(0.3)
        while not mpm.events.empty():
            mpm.events.get_nowait()
        mpm.cmd(f"rx {TEST_LEN}")
        time.sleep(a.count * 0.004)
        mpm.cmd("stop")
        pkts = []
        while not mpm.events.empty():
            e = mpm.events.get_nowait()
            if not e.startswith("@rx"):
                continue
            f = e.split()
            if "crc=bad" in e or len(f) < 4:
                continue
            pkts.append((int(f[1]), bytes.fromhex(f[3])))
    if not pkts:
        print("no packets")
        return 1
    bad = [d for _, d in pkts if channels_decode(d[4:26]) != chans or d[2] != 3 or d[3] != a.sub]
    seqs = [d[0] for _, d in pkts]
    lost = sum((b - a_) % 256 - 1 for a_, b in zip(seqs, seqs[1:]))
    cb = [struct.unpack("<I", d[26:30])[0] for _, d in pkts]
    # Callback periods between consecutive packets (no loss)
    per = [(cb[i + 1] - cb[i]) & 0xFFFFFFFF for i in range(len(pkts) - 1) if (seqs[i + 1] - seqs[i]) % 256 == 1]
    rx = [pkts[i + 1][0] - pkts[i][0] for i in range(len(pkts) - 1) if (seqs[i + 1] - seqs[i]) % 256 == 1]
    print(f"packets {len(pkts)}, lost {lost}, bad content {len(bad)}, flags 0x{pkts[-1][1][1]:02x}")
    if per:
        print(f"callback period us: min {min(per)} max {max(per)}")
        print(f"MPM RX period us:   min {min(rx)} max {max(rx)}")
    return 0 if pkts and not bad and per and max(per) - min(per) < 50 else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["status", "scan", "bind", "link"])
    ap.add_argument("-p", "--port", default="cc", help="serial port (default: the LaunchPad)")
    ap.add_argument("--proto", type=int, default=PROTO_TEST)
    ap.add_argument("--sub", type=int, default=0)
    ap.add_argument("--option", type=int, default=40)
    ap.add_argument("--time", type=float, default=2.0)
    ap.add_argument("--count", type=int, default=500)
    a = ap.parse_args()
    with Radio(find_port(a.port)) as r:
        rc = {"status": cmd_status, "scan": cmd_scan, "bind": cmd_bind, "link": cmd_link}[a.command](r, a)
        print(f"frames sent {r.frames_sent}")
    sys.exit(rc)


if __name__ == "__main__":
    main()
