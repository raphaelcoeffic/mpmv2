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
  sdr.py survey                     unknown transmitter: sweep 2400 to 2484 MHz and
                                    report channels, bit rate, sync word, payload
  sdr.py survey 2411 --n 200 -v     one window (2405 to 2417 MHz), each packet
  sdr.py v2x2 2440 --secs 60        V2x2 packets in one window: whole payload,
                                    CRC, decoded fields, then the bytes that moved

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
import random
import sys
import time
import zlib
from collections import Counter

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


def bursts(iq: np.ndarray, thr: float, min_len: int) -> list[tuple[int, int]]:
    """Every stretch above the threshold of at least min_len samples: (start, end)."""
    idx = np.flatnonzero(np.convolve(np.abs(iq) > thr, np.ones(64) / 64, "same") > 0.5)
    if len(idx) < min_len:
        return []
    br = np.flatnonzero(np.diff(idx) > 1)
    starts, ends = np.r_[idx[0], idx[br + 1]], np.r_[idx[br], idx[-1]]
    return [(int(a), int(b)) for a, b in zip(starts, ends) if b - a >= min_len]


def burst(iq: np.ndarray, thr: float | None = None, min_len: int = 2000) -> tuple[int, int] | None:
    """Longest stretch above the threshold: (start, end) samples."""
    a = np.abs(iq)
    # Not from the median: a packet can fill most of the burst
    thr = thr or max(0.3 * float(np.percentile(a, 99)), 6 * float(np.percentile(a, 5)), 10)
    b = bursts(iq, thr, min_len)
    return max(b, key=lambda x: x[1] - x[0]) if b else None


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


# Survey: sync words recognised in the sliced bits, as sent on air (BLE: LSB
# first). pre: an alternating byte must come before it (the end of a preamble).
SIGNATURES = [
    ("A7105 ID 5475c52a: FlySky, AFHDS2A, Kyosho, WFLY2, Joysway, Bugs", "5475c52a", True, False),
    ("A7105 ID 55201041: Hubsan", "55201041", True, False),
    ("A7105 ID 25a53c45: Height", "25a53c45", True, False),
    ("A7105 ID 06230623: Pelikan", "06230623", True, False),
    ("CC2500 sync d391: FrSky D/V/X, HoTT (inverted)", "d391", True, False),
    ("XN297 preamble 710f55: XN297 toys (Bayang, XK, WLtoys cars...)", "710f55", False, False),
    # Address 66 88 68 68 68, sent last byte first
    ("nRF24 address 6688686868: V2x2 (WLtoys V2x2, K969/K979/K989/K999 cars)", "6868688866", True, False),
    ("BLE advertising", "d6be898e", True, True),
]
RATE_CLASSES = (31.25e3, 62.5e3, 125e3, 250e3, 500e3, 1e6, 2e6)  # +-40% each
SURVEY_MIN_US = 40
RING = (0.8e6, 6.0e6)  # carrier offsets kept: away from DC and the band edges
DUMP = 8  # bytes shown after the sync word or the preamble
XN297_SCRAMBLE = bytes.fromhex("e3b14bea85")  # first bytes of the XN297 whitening (XN297_SCRAMBLE_SEED, iface_xn297.h)


def sig_bits(hexs: str, pre: bool, lsb: bool) -> list[int]:
    bits = to_bits(bytes.fromhex(hexs))
    if lsb:
        bits = [b for i in range(0, len(bits), 8) for b in bits[i:i + 8][::-1]]
    if pre:  # alternating, ending with the inverse of the first sync bit
        bits = [bits[0] ^ (k & 1) for k in range(8, 0, -1)] + bits
    return bits


def centre(f: np.ndarray) -> float:
    """Midway between the two tones: unlike the median, right when the data
    stays on one tone most of the time (unwhitened zeros)."""
    return float(np.percentile(f, 10) + np.percentile(f, 90)) / 2


def transitions(f: np.ndarray) -> np.ndarray:
    """Zero crossings with hysteresis (+-0.4 x the median level): sample times."""
    c = f - centre(f)
    lvl = 0.4 * float(np.median(np.abs(c)))
    s = np.where(c > lvl, 1, np.where(c < -lvl, -1, 0))
    idx = np.flatnonzero(s)
    ch = np.flatnonzero(np.diff(s[idx]) != 0)
    return (idx[ch] + idx[ch + 1]) / 2


def clock(tr: np.ndarray, grid: np.ndarray) -> tuple[float, float, float]:
    """(coherence, samples per bit, transition phase) of the transitions, best
    of the grid. 0 when not significant: N x coherence^2 >= 6 (Rayleigh test,
    noise alone stays near ln(grid points / transitions spacing) ~ 3)."""
    if len(tr) < 6:
        return 0.0, float(grid[len(grid) // 2]), 0.0
    m = np.exp(2j * np.pi * tr[None, :] / grid[:, None]).mean(axis=1)
    k = int(np.argmax(np.abs(m)))
    c, sps = float(abs(m[k])), float(grid[k])
    if len(tr) * c * c < 6:
        c = 0.0
    return c, sps, float(np.angle(m[k]) / (2 * np.pi) % 1 * sps)


def rate_class(x: np.ndarray) -> tuple[float, float] | None:
    """(samples per bit, coherence) of the lowest rate class whose transitions
    line up: the multiples of the bit rate line up too."""
    res = []
    for r in RATE_CLASSES:
        sps0 = FS / r
        f, _ = demod(x, r)
        e = int(sps0)
        tr = transitions(f[e:-e]) if len(f) > 4 * e else np.empty(0)
        tr = tr[tr < (tr[0] if len(tr) else 0) + 256 * 1.4 * sps0]  # coherent over 256 bits
        c, sps, _ = clock(tr, np.linspace(0.7 * sps0, 1.4 * sps0, 1000))
        res.append((sps, c))
    best = max(c for _, c in res)
    if best < 0.5:
        return None
    return next((sps, c) for sps, c in res if c >= 0.8 * best)


def analyze(x: np.ndarray, dump: int = DUMP) -> dict:
    """One burst: carrier offset, bit rate, deviation, bits, sync word."""
    xc = np.conj(x)
    f0 = centre(fm(xc))
    p = np.abs(np.fft.fft(xc)) ** 2
    fr = np.fft.fftfreq(len(xc), 1 / FS)
    r = {"f0": f0, "narrow": float(p[np.abs(fr - f0) < 1.5e6].sum() / p.sum())}
    if r["narrow"] < 0.6 or not RING[0] <= abs(f0) <= RING[1]:
        return r
    rc = rate_class(x)
    if not rc:
        return r
    f, f0 = demod(x, FS / rc[0])
    e = int(rc[0])
    f = f[e:-e] - (m := centre(f[e:-e]))
    r["f0"] = f0 + m
    coh, sps, ph = clock(transitions(f), np.linspace(0.99 * rc[0], 1.01 * rc[0], 401))
    v = slice_bits(f, sps, ph)
    if coh < 0.5 or len(v) < 16:
        return r
    r["coh"], r["rate"] = coh, FS / sps
    r["dev"] = float(np.median(np.abs(v)))
    bits = (v > 0).astype(int)
    for name, hexs, pre, lsb in SIGNATURES:
        sb = sig_bits(hexs, pre, lsb)
        i, inv = find_bits(bits, sb)
        if i is not None:
            body = (1 - bits if inv else bits)[i + len(sb):i + len(sb) + 8 * dump]
            r["kind"], r["dump"] = name, pack(body, lsb)
            if hexs == "710f55" and len(body) >= 40:
                r["dump"] += f"  addr {xn297_addr(r['dump'])}"
            return r
    # Unknown: the first 8+ bit alternating run (nRF24: a single preamble byte), bits after it
    alt = np.r_[False, bits[1:] != bits[:-1]]
    run = 0
    for i, a in enumerate(alt):
        run = run + 1 if a else 0
        if run >= 7 and (i + 1 == len(bits) or not alt[i + 1]):
            r["kind"] = f"unknown, {fmt_rate(round2(r['rate']))}"
            r["dump"] = pack(bits[i + 1:i + 1 + 8 * DUMP])
            return r
    r["kind"] = f"unknown, {fmt_rate(round2(r['rate']))}, no preamble"
    r["dump"] = pack(bits[:8 * DUMP])
    return r


def xn297_addr(dump: str) -> str:
    """The 5 address bytes after the XN297 preamble as given to XN297_SetTXAddr(),
    scrambled or not (sent last byte first). A 3 or 4 byte address: the first
    bytes of these are the payload."""
    a = bytes.fromhex(dump)[:5]
    scr = bytes(x ^ k for x, k in zip(a, XN297_SCRAMBLE))[::-1]
    return f"{scr.hex(' ')} (scrambled) / {a[::-1].hex(' ')} (not)"


def pack(bits: np.ndarray, lsb: bool = False) -> str:
    out = []
    for j in range(0, len(bits) - 7, 8):
        b = bits[j:j + 8][::-1] if lsb else bits[j:j + 8]
        out.append(f"{int(''.join(map(str, b)), 2):02x}")
    return " ".join(out)


def round2(r: float) -> float:
    """Two significant digits: the unknown packets are grouped by rate."""
    return float(f"{r:.2g}")


def fmt_rate(r: float) -> str:
    return f"{r / 1e6:.3g} Mbps" if r >= 999.5e3 else f"{r / 1e3:.3g} kbps"


def survey(sdr: Sdr, centres: list[int], n: int, verbose: bool):
    recs, other, floor = [], Counter(), np.inf
    # The 16 MS/s captures keep every fifth sample: what the analog filter
    # passes beyond +-8 MHz folds into the window (default: about 69 MHz wide)
    sdr.cmd("BANDWIDTH 13")
    for c in centres:
        sdr.cmd(f"FREQ {c}")
        for k in range(n):
            time.sleep(random.uniform(0, 0.003))  # else the captures can lock to the TX period
            iq = sdr.cap()
            iq = iq - iq.mean()
            # Noise floor: the quietest capture so far (a slow packet fills a capture)
            floor = min(floor, float(np.percentile(np.abs(iq), 10)))
            thr = max(5 * floor, 8)
            for b0, b1 in bursts(iq, thr, int(SURVEY_MIN_US * 1e-6 * FS)):
                x = iq[b0:b1]
                r = analyze(x)
                if r["narrow"] < 0.6:
                    other["wideband (Wi-Fi?)"] += 1
                elif not RING[0] <= abs(r["f0"]) <= RING[1]:
                    other["outside the window (seen from the next one)"] += 1
                elif "rate" not in r:
                    other["narrowband, no bit clock (DSSS? noise?)"] += 1
                elif b0 < 64 and r["kind"].startswith("unknown") and r["coh"] < 0.8:
                    other["cut before their sync word"] += 1
                else:
                    if b0 < 64 and r["kind"].startswith("unknown"):  # a slow one, longer than the capture
                        r["kind"], r["dump"] = r["kind"].split(", no preamble")[0], "(started before the capture)"
                    r.update(cap=(c, k), t0=b0 / FS * 1e6, t1=b1 / FS * 1e6, mhz=c + r["f0"] / 1e6,
                             cut=b0 < 64 or b1 > len(iq) - 64, level=float(np.abs(x).mean()),
                             clip=clipping(x))
                    recs.append(r)
                    if verbose:
                        print(f"{r['mhz']:8.2f} MHz  {r['t0']:5.0f} us +{r['t1'] - r['t0']:4.0f}"
                              f"{'+' if r['cut'] else ' '} {fmt_rate(r['rate']):>10} coh {r['coh']:.2f}"
                              f" dev {r['dev'] / 1e3:3.0f} kHz |iq| {r['level']:3.0f}  {r['kind'][:24]:24}"
                              f"  {r['dump']}", flush=True)
        if not verbose:
            print(f"{c} MHz: {sum(r['cap'][0] == c for r in recs)} packets", file=sys.stderr, flush=True)

    print(f"\n{len(centres)} windows x {n} captures: {len(recs)} packets"
          + "".join(f", {v} {k}" for k, v in other.items()))
    kinds = Counter(r["kind"] for r in recs)
    for kind, cnt in kinds.most_common():
        rs = [r for r in recs if r["kind"] == kind]
        whole = [r["t1"] - r["t0"] for r in rs if not r["cut"]]
        print(f"\n{kind}: {cnt} packets" + multiple(kind, kinds, recs))
        print(f"  rate {fmt_rate(float(np.median([r['rate'] for r in rs])))}, deviation "
              f"{np.median([r['dev'] for r in rs]) / 1e3:.0f} kHz, coherence {np.median([r['coh'] for r in rs]):.2f}")
        print("  length " + (f"{np.median(whole):.0f} us ({min(whole):.0f} to {max(whole):.0f})"
                             if whole else "-") + f", {cnt - len(whole)} cut by the capture")
        print(f"  {len(chans := channels([r['mhz'] for r in rs]))} channels (MHz, packets): "
              + " ".join(f"{m:.2f}({k})" for m, k in chans))
        gaps = [b["t0"] - a["t0"] for a, b in zip(rs, rs[1:]) if a["cap"] == b["cap"] and a["t0"] > 4]
        if gaps:
            print(f"  spacing in one capture: {' '.join(f'{g:.0f}' for g in sorted(gaps)[:12])} us")
        dumps = Counter(r["dump"] for r in rs)
        print(f"  after the sync / preamble ({len(dumps)} different):")
        for d, k in dumps.most_common(6):
            print(f"    {k:4}x {d}")


V2X2_ADDR = bytes.fromhex("6868688866")  # 66 88 68 68 68 as sent, last byte first


def crc16_nrf(data: bytes) -> int:
    """nRF24L01 2 byte CRC (CCITT, init 0xffff) over the address and the payload."""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021 if crc & 0x8000 else crc << 1) & 0xFFFF
    return crc


def v2x2_fields(p: bytes) -> str:
    """V2X2_send_packet(): throttle, rudder, elevator, aileron (sign + magnitude
    for the last 3), 3 trims, TX id, flags2, flags, checksum."""
    s8 = lambda b: -(b & 0x7F) if b & 0x80 else b
    ok = "ok" if sum(p[:15]) & 0xFF == p[15] else "BAD"
    return (f"T {p[0]:3} R {s8(p[1]):+4} E {s8(p[2]):+4} A {s8(p[3]):+4}  trim {p[4]:02x} {p[5]:02x} {p[6]:02x}"
            f"  id {p[7:10].hex()}  f2 {p[10]:02x} {p[11:14].hex()}  flags {p[14]:02x}  sum {ok}")


def v2x2(sdr: Sdr, centre: int, secs: float):
    """V2x2 packets in one window until secs or Ctrl-C: one line per CRC checked
    packet, then the range of each payload byte."""
    sdr.cmd("BANDWIDTH 13")
    sdr.cmd(f"FREQ {centre}")
    t_start, floor, seen, bad = time.time(), np.inf, [], 0
    try:
        while time.time() - t_start < secs:
            time.sleep(random.uniform(0, 0.003))
            iq = sdr.cap()
            iq = iq - iq.mean()
            floor = min(floor, float(np.percentile(np.abs(iq), 10)))
            for b0, b1 in bursts(iq, max(5 * floor, 8), int(SURVEY_MIN_US * 1e-6 * FS)):
                r = analyze(iq[b0:b1], dump=18)
                if not r.get("kind", "").startswith("nRF24 address 6688686868") or not RING[0] <= abs(r["f0"]) <= RING[1]:
                    continue
                d = bytes.fromhex(r["dump"])
                if len(d) < 18 or crc16_nrf(V2X2_ADDR + d[:16]) != int.from_bytes(d[16:18], "big"):
                    bad += 1
                    continue
                seen.append(d[:16])
                print(f"{time.time() - t_start:6.1f} s {centre + r['f0'] / 1e6:8.2f} MHz  {d[:16].hex(' ')}"
                      f"  {v2x2_fields(d[:16])}", flush=True)
    except KeyboardInterrupt:
        pass
    print(f"\n{len(seen)} packets with a good CRC, {bad} cut or bad")
    if seen:
        a = np.frombuffer(b"".join(seen), np.uint8).reshape(-1, 16)
        print("byte  min max  values")
        for i in range(16):
            v = sorted(set(a[:, i].tolist()))
            print(f"  {i:2}   {v[0]:02x}  {v[-1]:02x}  {len(v)}" + (f"  ({' '.join(f'{x:02x}' for x in v)})" if len(v) <= 8 else ""))


def multiple(kind: str, kinds: Counter, recs: list[dict]) -> str:
    """Unknown packets at a multiple of the rate of more common ones: often
    the same signal (a slow one with few transitions in the capture)."""
    if not kind.startswith("unknown") or not all(r["cut"] for r in recs if r["kind"] == kind):
        return ""
    def rate(k: str) -> float:
        return float(np.median([r["rate"] for r in recs if r["kind"] == k]))

    for k, n in kinds.most_common():
        if n <= kinds[kind] or not k.startswith("unknown"):
            continue
        m = rate(kind) / rate(k)
        if 1.5 < m < 4.5 and abs(m - round(m)) < 0.02 * m:
            return f" (x{round(m)} the rate of the {fmt_rate(rate(k))} ones: probably those)"
    return ""


def channels(mhz: list[float], gap: float = 0.25) -> list[tuple[float, int]]:
    """Frequencies grouped where they are less than gap MHz apart: (mean, count)."""
    out, cur = [], []
    for m in sorted(mhz):
        if cur and m - cur[-1] > gap:
            out.append((float(np.mean(cur)), len(cur)))
            cur = []
        cur.append(m)
    if cur:
        out.append((float(np.mean(cur)), len(cur)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", choices=("info", "cap", "survey", "v2x2"))
    ap.add_argument("freq", nargs="*", type=int, help="MHz: cap (default 2442), survey window centres "
                    "(default 2401 to 2481 every 5 MHz, each covers 0.8 to 6 MHz on both sides)")
    ap.add_argument("--gain", default="40", help="manual gain index, or 'auto' (hardware AGC)")
    ap.add_argument("--n", type=int, default=20, help="survey: captures per window")
    ap.add_argument("-v", action="store_true", help="survey: each packet")
    ap.add_argument("--secs", type=float, default=60, help="v2x2: capture time")
    a = ap.parse_args()
    with Sdr() as sdr:
        if a.what == "info":
            for c in ("INFO", "VERSION?", "TRANSPORT?", "LIMITS?", "GAIN?"):
                print(f"{c:11} {sdr.cmd(c)}")
            return 0
        sdr.cmd("GAIN HARDWARE" if a.gain == "auto" else f"GAIN MANUAL {a.gain}")
        if a.what == "v2x2":
            try:
                v2x2(sdr, a.freq[0] if a.freq else 2440, a.secs)
            finally:
                sdr.cmd("BANDWIDTH 0")
            return 0
        if a.what == "survey":
            try:
                survey(sdr, a.freq or list(range(2401, 2482, 5)), a.n, a.v)
            finally:
                sdr.cmd("BANDWIDTH 0")  # the default, as the other tools expect
            return 0
        sdr.cmd(f"FREQ {a.freq[0] if a.freq else 2442}")
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
