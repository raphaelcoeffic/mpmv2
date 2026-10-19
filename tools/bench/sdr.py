#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5", "numpy"]
# ///
"""ESP32-S3 running esp-sdr (https://github.com/ESPARGOS/esp-sdr) as a third
receiver on the bench: I/Q bursts of up to 16380 samples (about 1 ms at
16 MS/s), FM demodulation, bit clock recovery and slicing.

  sdr.py info                       firmware, limits, gain
  sdr.py cap 2485 --gain 40         one burst at 2485 MHz: level, clipping, carrier

Measured on the bench (ESP32-S3, esp-sdr f86da81): the spectrum is inverted
(demod() conjugates), FREQ takes whole MHz, so tune 1 MHz off the signal to
keep it away from the DC spike. The hardware AGC only catches the start of a
packet: use a manual gain (index, not dB), about 40 at 1 m from the 4-in-1,
35 from the MPM v2 at full power. The ESP32 crystal makes absolute
frequencies good to about 10 to 20 ppm only: compare signals, don't measure
them. Port: $SDR_PORT, else the first Espressif USB Serial/JTAG device.
"""

import argparse
import os
import sys
import time
import zlib

import numpy as np
import serial
from serial.tools import list_ports

FS = 16e6
RATE_16M = 6  # CAP16 rate code for 16 MS/s
ESPRESSIF_JTAG = (0x303A, 0x1001)


def find_port() -> str:
    if os.environ.get("SDR_PORT"):
        return os.environ["SDR_PORT"]
    for p in list_ports.comports():
        if (p.vid, p.pid) == ESPRESSIF_JTAG:
            return p.device
    raise SystemExit("No esp-sdr board found, set $SDR_PORT")


class Sdr:
    def __init__(self, port: str | None = None):
        self.s = serial.Serial(port or find_port(), 2000000, timeout=3)
        time.sleep(0.3)
        self.s.reset_input_buffer()
        self.s.write(b"SYNC 77\n")
        deadline = time.time() + 5
        while self.s.readline().strip() != b"SYNC 77":
            if time.time() > deadline:
                raise RuntimeError("esp-sdr: no SYNC reply")

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        try:
            self.cmd("RELEASE")
        except Exception:
            pass
        self.s.close()

    def cmd(self, c: str) -> str:
        self.s.write((c + "\n").encode())
        r = self.s.readline().decode().strip()
        if r.startswith("ERR"):
            raise RuntimeError(f"{c}: {r}")
        return r

    def cap(self, n: int = 16380, rate_code: int = RATE_16M) -> np.ndarray:
        r = self.cmd(f"CAP16 {n} {rate_code}").split()
        if r[0] != "DATA":
            raise RuntimeError(f"CAP16: {r}")
        n, crc = int(r[1]), int(r[2], 16)
        data = self.s.read(2 * n)
        if zlib.crc32(data) & 0xFFFFFFFF != crc:
            raise RuntimeError("CAP16: CRC error")
        iq = np.frombuffer(data, np.int8).astype(np.float32)
        return iq[0::2] + 1j * iq[1::2]

    def autogain(self, start: int = 70, tries: int = 8, min_len: int = 2000) -> int | None:
        """Highest manual gain without clipping on the packets being sent."""
        for g in range(start, 0, -5):
            self.cmd(f"GAIN MANUAL {g}")
            lv = []
            for _ in range(tries):
                b = burst(iq := self.cap(), thr=10, min_len=min_len)
                if b:
                    lv.append(clipping(iq[b[0]:b[1]]))
            if lv and max(lv) < 0.002:
                return g
        return None


def clipping(x: np.ndarray) -> float:
    return float(np.mean((np.abs(x.real) >= 126) | (np.abs(x.imag) >= 126)))


def burst(iq: np.ndarray, thr: float | None = None, min_len: int = 2000) -> tuple[int, int] | None:
    """Longest stretch above the threshold: (start, end) samples."""
    a = np.abs(iq)
    # Not from the median: a packet can fill most of the burst
    thr = thr or max(0.3 * float(np.percentile(a, 99)), 6 * float(np.percentile(a, 5)), 10)
    idx = np.flatnonzero(np.convolve(a > thr, np.ones(64) / 64, "same") > 0.5)
    if len(idx) < min_len:
        return None
    br = np.flatnonzero(np.diff(idx) > 1)
    starts, ends = np.r_[idx[0], idx[br + 1]], np.r_[idx[br], idx[-1]]
    k = int(np.argmax(ends - starts))
    return (int(starts[k]), int(ends[k])) if ends[k] - starts[k] >= min_len else None


def fm(iq: np.ndarray) -> np.ndarray:
    return np.angle(iq[1:] * np.conj(iq[:-1])) * FS / (2 * np.pi)


def demod(iq: np.ndarray, rate: float, invert: bool = True) -> tuple[np.ndarray, float]:
    """Burst -> (instantaneous frequency in Hz, carrier offset in Hz from the
    tuned frequency). The carrier is mixed to 0 and low-pass filtered first."""
    x = np.conj(iq) if invert else iq
    f0 = float(np.median(fm(x)))
    x = x * np.exp(-2j * np.pi * f0 / FS * np.arange(len(x)))
    n = max(int(FS / rate / 2), 2)
    return fm(np.convolve(x, np.ones(n) / n, "same")), f0


def bit_clock(f: np.ndarray, rate: float) -> tuple[float, float, float]:
    """(coherence 0..1, samples per bit, transition phase in samples) from the
    zero crossings, searched within +-0.5% of the nominal rate."""
    tr = np.flatnonzero(np.diff(np.sign(f - np.median(f))) != 0).astype(float)
    best = (0.0, FS / rate, 0.0)
    for sps in np.linspace(FS / rate * 0.995, FS / rate * 1.005, 201):
        m = np.exp(2j * np.pi * tr / sps).mean()
        if abs(m) > best[0]:
            best = (float(abs(m)), float(sps), float(np.angle(m) / (2 * np.pi) % 1 * sps))
    return best


def slice_bits(f: np.ndarray, sps: float, tphase: float) -> np.ndarray:
    """Frequency at each bit centre."""
    centres = np.arange(tphase + sps / 2, len(f) - 1, sps)
    return np.interp(centres, np.arange(len(f)), f)


def to_bits(data: bytes) -> list[int]:
    return [(b >> (7 - k)) & 1 for b in data for k in range(8)]


def find_bits(bits: np.ndarray, pattern: list[int]) -> tuple[int | None, bool]:
    """First position of pattern (or of its inverse): (index, inverted)."""
    p, n = np.array(pattern), len(pattern)
    for i in range(len(bits) - n + 1):
        w = bits[i:i + n]
        if (w == p).all():
            return i, False
        if (w != p).all():
            return i, True
    return None, False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", choices=("info", "cap"))
    ap.add_argument("freq", nargs="?", type=int, default=2442, help="MHz (cap)")
    ap.add_argument("--gain", default="40", help="manual gain index, or 'auto' (hardware AGC)")
    a = ap.parse_args()
    with Sdr() as sdr:
        if a.what == "info":
            for c in ("INFO", "VERSION?", "TRANSPORT?", "LIMITS?", "GAIN?"):
                print(f"{c:11} {sdr.cmd(c)}")
            return 0
        sdr.cmd(f"FREQ {a.freq}")
        sdr.cmd("GAIN HARDWARE" if a.gain == "auto" else f"GAIN MANUAL {a.gain}")
        iq = sdr.cap()
        b = burst(iq)
        print(f"|iq| median {np.median(np.abs(iq)):.1f} max {np.abs(iq).max():.0f}, clipping "
              f"{100 * clipping(iq):.1f}%")
        if b:
            x = iq[b[0]:b[1]]
            print(f"burst {b[0] / FS * 1e6:.0f} to {b[1] / FS * 1e6:.0f} us, |iq| {np.abs(x).mean():.0f}, "
                  f"clipping {100 * clipping(x):.1f}%, carrier {np.median(fm(np.conj(x))) / 1e3:+.0f} kHz")
        else:
            print("no burst")
    return 0


if __name__ == "__main__":
    sys.exit(main())
