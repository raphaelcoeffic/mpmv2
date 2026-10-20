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
  radio_sim.py drive --proto 25 [--bind [--bind-time 60] [--options -90:90:10]] [--option 0] [--time 0]
                                         a real receiver: CH1-4 sweep (--sweep-ch), status and telemetry each second
"""

from __future__ import annotations

import argparse
import math
import queue
import struct
import sys
import threading
import time
from collections import Counter
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


def telem_decode(ty: int, d: bytes) -> dict[str, int]:
    """The link statistics of the MPM's FrSky telemetry frames (telemetry.ino):
    S.Port (type 2) RSSI 0xF101 and battery 0xF104, hub (type 3) link frame 0xFE,
    DSM (type 4) QoS / TM1000 and bind response (type 5), AFHDS2A (type 6) sensors.
    RX_RSSI: the receiver's, TX_RSSI: the module's (raw CC2500 RSSI + 128),
    TX_LQI: telemetry packets in 900 ms (FrSky X), the CC2500 LQI (FrSky D)."""
    if ty == 0x02 and len(d) >= 8:
        appid = d[2] | d[3] << 8
        if appid == 0xF101:
            return {"rx_rssi": d[4], "tx_rssi": d[5], "rx_lqi": d[6], "tx_lqi": d[7]}
        if appid == 0xF104:
            return {"a1": d[4]}
    elif ty == 0x03 and len(d) >= 7 and d[0] == 0xFE:
        return {"a1": d[1], "a2": d[2], "rx_rssi": d[3], "tx_rssi": d[4], "rx_lqi": d[5], "tx_lqi": d[6]}
    elif ty == 0x04 and len(d) >= 17:
        # DSM: CYRF RSSI of the telemetry, then the Spektrum packet (I2C address first)
        out = {"tx_rssi": d[0]}
        if d[1] == 0x7F:  # QoS: fades A, B, L, R, frame losses, holds (big endian), RX voltage 0.01 V
            for k, v in zip(("fades_a", "fades_b", "fades_l", "fades_r", "losses", "holds", "rx_v"),
                            struct.unpack(">7H", d[3:17])):
                out[k] = v
        elif d[1] == 0x7E:  # TM1000: RPM period, voltage 0.01 V, temperature F
            out.update(zip(("rpm", "volt", "temp_f"), struct.unpack(">3H", d[3:9])))
        else:
            out[f"sensor_{d[1]:02x}"] = 1
        return out
    elif ty == 0x05 and len(d) >= 10:
        # DSM bind response: channels, mode (0xa2 DSMX 22 ms, 0xb2 DSMX 11 ms, 0x01/0x02/0x12 DSM2)
        return {"bind_ch": d[5], "bind_mode": d[6]}
    elif ty == 0x06 and len(d) >= 29:
        # AFHDS2A (option bit 7): TX_RSSI, 7 sensors: id, instance, value (16 bit LE)
        out = {"tx_rssi": d[0]}
        for i in range(1, 29, 4):
            sid, val = d[i], int.from_bytes(d[i + 2:i + 4], "little", signed=True)
            if sid in AFHDS2A_SENSORS:
                out[AFHDS2A_SENSORS[sid]] = val
            elif sid == 0xFF:
                break
        return out
    return {}


# AFHDS2A sensor ids (AFHDS2A_a7105.c): RX voltage in 1/100 V, error rate in %
AFHDS2A_SENSORS = {0x00: "rx_v", 0x03: "a3_v", 0xFE: "err", 0xFC: "rx_rssi", 0xFB: "noise", 0xFA: "snr"}


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
    end = time.monotonic() + (a.time or 2.0)
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


def cmd_drive(r: Radio, a):
    """A real receiver: with --bind the bind flag stays set until the
    protocol ends its bind; --bind-time keeps binding that long, restarting
    the bind (the flag cleared for a few frames) every --step seconds or when
    the protocol ends it, with the next of --options if given. Then the first
    --sweep-ch channels sweep (spread over the period), the others centred.
    Status flags, telemetry frame counts and link statistics each second, until --time (0: Ctrl-C)."""
    options = [a.option]
    if a.options:
        lo, hi, st = (int(v) for v in a.options.split(":"))
        options = list(range(lo, hi + 1, st))
    r.set(protocol=a.proto, sub=a.sub, option=options[0], rx_num=a.rx_num, bind=a.bind)
    t0 = time.monotonic()
    binding = a.bind
    bind_start, opt_idx = 0.0, 0
    if binding:
        print(f"{0.0:6.1f} s bind, option {options[0]}")
    next_print = t0 + 1.0
    counts: Counter = Counter()
    flags: list[str] = []
    link: dict[str, int] = {}
    try:
        while not a.time or time.monotonic() - t0 < a.time:
            t = time.monotonic() - t0
            chans = [CH_CENTER] * 16
            if not binding:
                for i in range(a.sweep_ch):
                    chans[i] = round(CH_CENTER + (CH_MAX - CH_CENTER) *
                                     math.sin(2 * math.pi * (t / a.period - i / a.sweep_ch)))
            r.set(channels=chans)
            while not r.telem.empty():
                _, ty, d = r.telem.get_nowait()
                counts[ty] += 1
                if ty == 0x01:
                    flags = status_decode(d)["flags"]
                link.update(telem_decode(ty, d))
                if ty == 0x05 and a.proto == 6 and a.sub == 4 and len(d) >= 7:
                    # As EdgeTX (DSM Auto): the receiver's mode and channel count
                    # once the bind is over (a model change restarts the protocol)
                    sub = {0xA2: 2, 0x01: 0, 0x02: 0, 0x12: 1}.get(d[6], 3)
                    ch = min(max(d[5], 3), 12)
                    a.sub, a.option, a.bind_time = sub, (a.option & 0xF0) | ch, 0
                    print(f"{t:6.1f} s DSM bind response {d[:10].hex()}: sub {sub}, {ch} channels")
            if binding and t - bind_start > 1.0:
                ended = "binding" not in flags
                if t < a.bind_time and (ended or (a.options and t - bind_start >= a.step)):
                    opt_idx = (opt_idx + 1) % len(options) if a.options else 0
                    r.set(bind=False)
                    time.sleep(0.03)
                    r.set(bind=True, option=options[opt_idx])
                    bind_start, flags = t, []
                    print(f"{t:6.1f} s bind, option {options[opt_idx]}")
                elif ended or t >= a.bind_time > 0:
                    r.set(bind=False, option=a.option, sub=a.sub)
                    binding = False
                    print(f"{t:6.1f} s bind done, option {a.option}")
            if time.monotonic() >= next_print:
                next_print += 1.0
                telem = " ".join(f"{ty:02x}:{n}" for ty, n in sorted(counts.items()))
                stats = " ".join(f"{k} {v}" for k, v in link.items())
                print(f"{t:6.1f} s {','.join(flags):30s} ch {chans[:max(4, a.sweep_ch)]} telemetry {telem} {stats}", flush=True)
                counts.clear()
                link.clear()
            time.sleep(0.02)
    except KeyboardInterrupt:
        pass
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["status", "scan", "bind", "link", "drive"])
    ap.add_argument("-p", "--port", default="cc", help="serial port (default: the LaunchPad)")
    ap.add_argument("--proto", type=int, default=PROTO_TEST)
    ap.add_argument("--sub", type=int, default=0)
    ap.add_argument("--option", type=int, help="default: 40 for the test protocol (its RF channel), else 0")
    ap.add_argument("--time", type=float, help="seconds (default 2, drive: until Ctrl-C)")
    ap.add_argument("--count", type=int, default=500)
    ap.add_argument("--rx-num", type=int, default=0)
    ap.add_argument("--bind", action="store_true", help="drive: bind first")
    ap.add_argument("--period", type=float, default=4.0, help="drive: sweep period, s")
    ap.add_argument("--sweep-ch", type=int, default=4, help="drive: channels swept (from CH1)")
    ap.add_argument("--bind-time", type=float, default=0.0, help="drive: keep binding, s")
    ap.add_argument("--options", help="drive: option sweep while binding, lo:hi:step")
    ap.add_argument("--step", type=float, default=3.0, help="drive: seconds per --options value")
    a = ap.parse_args()
    if a.option is None:
        a.option = 40 if a.proto == PROTO_TEST else 0
    with Radio(find_port(a.port)) as r:
        rc = {"status": cmd_status, "scan": cmd_scan, "bind": cmd_bind, "link": cmd_link,
              "drive": cmd_drive}[a.command](r, a)
        print(f"frames sent {r.frames_sent}")
    sys.exit(rc)


if __name__ == "__main__":
    main()
