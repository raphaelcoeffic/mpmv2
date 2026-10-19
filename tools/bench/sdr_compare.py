#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5", "numpy"]
# ///
"""The same A7105 packet sent by the MPM A7105 and by the CC2652, received by
the esp-sdr board (sdr.py): bit rate, carrier, deviation in runs and on
isolated bits, worst bit, payload decoded from the SDR bits.

  sdr_compare.py flysky
  sdr_compare.py joysway --bits 48 72          per-bit levels, bits 48 to 71
                                               after the ID start - 16
  sdr_compare.py wfly2 --cc-pwr 0x04c6

Uses the suite_a7105.py setups, ch 170 (2485 MHz). Both senders send every
1.5 ms so that each 1 ms burst holds a whole packet now and then.
"""

import argparse
import random
import sys
import time

import numpy as np

import rig
import sdr as S
import suite_a7105

PRESET_ID = {"flysky": "5475c52a", "afhds2a": "5475c52a", "hubsan": "55201041"}


def a7105_mhz(ch: int) -> float:
    return 2400 + ch * 0.5


def packets(sdr: S.Sdr, rate: float, sync: bytes, n: int, want: int, tries: int = 80):
    """Decoded packets: (carrier Hz, bit rate, frequency at each bit from 16
    bits before the sync, payload bytes)."""
    out = []
    min_len = 0.9 * (4 + len(sync) + n + 2) * 8 / rate * S.FS
    for _ in range(tries):
        time.sleep(random.uniform(0, 0.003))  # else the capture loop can lock to the TX period
        iq = sdr.cap()
        b = S.burst(iq)
        if not b or b[1] - b[0] < min_len:
            continue
        f, f0 = S.demod(iq[b[0]:b[1]], rate)
        _, sps, ph = S.bit_clock(f[50:-50], rate)
        v = S.slice_bits(f[50:-50], sps, ph)
        bits = (v > 0).astype(int)
        i, inv = S.find_bits(bits, S.to_bits(sync))
        if i is None or i < 16 or i + 8 * (len(sync) + n) > len(bits):
            continue
        if inv:
            v, bits = -v, 1 - bits
        body = bits[i + 8 * len(sync):i + 8 * (len(sync) + n)]
        payload = bytes(int("".join(map(str, body[j:j + 8])), 2) for j in range(0, 8 * n, 8))
        out.append((f0, rate * (S.FS / rate) / sps, v[i - 16:], payload))
        if len(out) >= want:
            break
    return out


def summary(label: str, gain, pk, sync: bytes, payload: bytes):
    if not pk:
        print(f"{label}: gain {gain}, no packet decoded")
        return
    n = len(payload)
    rows = []
    for f0, br, v, got in pk:
        exp = bytearray(payload)
        exp[rig.SEQ] = got[rig.SEQ]
        seg = v[:16 + 8 * (len(sync) + n)]
        bits = (seg > 0).astype(int)
        iso = [abs(seg[j]) for j in range(1, len(seg) - 1) if bits[j - 1] != bits[j] != bits[j + 1]]
        run = [abs(seg[j]) for j in range(1, len(seg) - 1) if bits[j - 1] == bits[j] == bits[j + 1]]
        rows.append((f0, br, np.mean(run), np.mean(iso), np.min(np.abs(seg)), got == bytes(exp)))
    r = np.array(rows, dtype=float)
    print(f"{label}: gain {gain}, {len(r)} packets, payload right {int(r[:, 5].sum())}/{len(r)}")
    print(f"   carrier {r[:, 0].mean() / 1e3:+.1f} kHz (sd {r[:, 0].std() / 1e3:.1f}), "
          f"bit rate {r[:, 1].mean():.0f} bps")
    print(f"   deviation: runs {r[:, 2].mean() / 1e3:.0f} kHz, isolated bits {r[:, 3].mean() / 1e3:.0f} kHz "
          f"({100 * r[:, 3].mean() / r[:, 2].mean():.0f}%), worst bit {r[:, 4].mean() / 1e3:.0f} kHz")


def bit_table(label: str, pk, sync: bytes, payload: bytes, lo: int, hi: int):
    exp = S.to_bits(b"\xaa\xaa" + sync + payload)
    v = np.array([p[2][:hi] for p in pk if len(p[2]) >= hi]) / 1e3
    print(f"{label}, frequency per bit (kHz), bit 16 = first sync bit:")
    for j in range(lo, hi):
        print(f"   bit {j:3d} ({exp[j]})  mean {v[:, j].mean():+6.0f}  worst {np.abs(v[:, j]).min():4.0f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("name", help=f"A7105 setup: {' '.join(suite_a7105.P)}")
    ap.add_argument("--ch", type=int, default=170)
    ap.add_argument("--trim", type=int, default=90)
    ap.add_argument("--count", type=int, default=8, help="packets per sender")
    ap.add_argument("--cc-pwr", default="default", help="CC2652 TX power word")
    ap.add_argument("--bits", nargs=2, type=int, metavar=("LO", "HI"), help="per-bit table")
    a = ap.parse_args()
    if a.name not in suite_a7105.P:
        ap.error(f"unknown setup {a.name}")
    n, id_, rate_code, _ = suite_a7105.P[a.name]
    sync = bytes.fromhex(id_ or PRESET_ID[a.name])
    rate = 500e3 / (rate_code + 1)
    mpm_tx, _, cc_setup, _ = suite_a7105.setups(a.name, a.trim)
    payload = rig.pattern(n)
    freq = a7105_mhz(a.ch)
    mpm, cc = rig.open_devices()
    with S.Sdr() as sdr, mpm, cc:
        sdr.cmd(f"FREQ {int(freq) - 1}")
        senders = (("MPM A7105", mpm, rig.commands(mpm_tx)),
                   ("CC2652", cc, rig.commands(cc_setup) + [f"pwr {a.cc_pwr}"] + rig.commands(cc_setup)))
        for label, dev, setup in senders:
            rig.run(dev, setup + [f"ch {a.ch}"])
            dev.cmd(f"txp 1500 60000 {payload.hex()}")
            try:
                gain = sdr.autogain()
                pk = packets(sdr, rate, sync, n, a.count)
                summary(label, gain, pk, sync, payload)
                if a.bits and pk:
                    bit_table(label, pk, sync, payload, *a.bits)
            finally:
                dev.cmd("stop")
        cc.cmd("pwr default")
    return 0


if __name__ == "__main__":
    sys.exit(main())
