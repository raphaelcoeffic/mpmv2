#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5", "numpy"]
# ///
"""The same packet sent by the MPM (A7105 or NRF24L01) and by the CC2652,
received by the esp-sdr board (sdr.py): bit rate, carrier, deviation in runs
and on isolated bits, worst bit, payload decoded from the SDR bits.

  sdr_compare.py flysky
  sdr_compare.py joysway --bits 48 72          per-bit levels, bits 48 to 71
                                               after the ID start - 16 (NRF: - 6)
  sdr_compare.py wfly2 --cc-pwr 0x04c6
  sdr_compare.py nrf2m --ch 40

Uses the suite_a7105.py setups, ch 170 (2485 MHz), or the plain NRF24L01
setups of suite_nrf.py (nrf1m, nrf250k, nrf2m), address e7e7e7e7e7, ch 40
(2440 MHz). Both senders send every 1.5 ms so that each 1 ms burst holds a
whole packet now and then.
"""

import argparse
import random
import sys
import time

import numpy as np

import rig
import sdr as S
import suite_a7105
import suite_nrf

PRESET_ID = {"flysky": "5475c52a", "afhds2a": "5475c52a", "hubsan": "55201041"}


def a7105_mhz(ch: int) -> float:
    return 2400 + ch * 0.5


def packets(sdr: S.Sdr, rate: float, sync: bytes, n: int, want: int, pre: int = 4, tries: int = 80):
    """Decoded packets: (carrier Hz, bit rate, frequency at each bit from
    back() bits before the sync, payload bytes). pre: preamble bytes."""
    out = []
    edge = int(1.5 * S.FS / rate)  # ramps at both ends
    min_len = int(0.9 * (pre + len(sync) + n + 2) * 8 / rate * S.FS)
    for _ in range(tries):
        time.sleep(random.uniform(0, 0.003))  # else the capture loop can lock to the TX period
        iq = sdr.cap()
        b = S.burst(iq, min_len=min_len)
        if not b:
            continue
        f, f0 = S.demod(iq[b[0]:b[1]], rate)
        _, sps, ph = S.bit_clock(f[edge:-edge], rate)
        v = S.slice_bits(f[edge:-edge], sps, ph)
        bits = (v > 0).astype(int)
        i, inv = S.find_bits(bits, S.to_bits(sync))
        if i is None or i < back(pre) or i + 8 * (len(sync) + n) > len(bits):
            continue
        if inv:
            v, bits = -v, 1 - bits
        body = bits[i + 8 * len(sync):i + 8 * (len(sync) + n)]
        payload = bytes(int("".join(map(str, body[j:j + 8])), 2) for j in range(0, 8 * n, 8))
        out.append((f0, rate * (S.FS / rate) / sps, v[i - back(pre):], payload))
        if len(out) >= want:
            break
    return out


def back(pre: int) -> int:
    """Preamble bits looked at before the sync: 16, or 6 of the single NRF
    preamble byte (its first bits fall in the ramp)."""
    return 16 if pre > 1 else 6


def summary(label: str, gain, pk, sync: bytes, payload: bytes, pre: int):
    if not pk:
        print(f"{label}: gain {gain}, no packet decoded")
        return
    n = len(payload)
    rows = []
    for f0, br, v, got in pk:
        exp = bytearray(payload)
        exp[rig.SEQ] = got[rig.SEQ]
        seg = v[:back(pre) + 8 * (len(sync) + n)]
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


def bit_table(label: str, pk, sync: bytes, payload: bytes, pre: int, lo: int, hi: int):
    exp = S.to_bits(bytes([0xaa if sync[0] & 0x80 else 0x55]) * 2 + sync + payload)[16 - back(pre):]
    v = np.array([p[2][:hi] for p in pk if len(p[2]) >= hi]) / 1e3
    print(f"{label}, frequency per bit (kHz), bit {back(pre)} = first sync bit:")
    for j in range(lo, hi):
        print(f"   bit {j:3d} ({exp[j]})  mean {v[:, j].mean():+6.0f}  worst {np.abs(v[:, j]).min():4.0f}")


def setup(name: str, ch: int | None, trim: int, addr: str):
    """(MPM TX setup, CC2652 setup, sync bytes, payload length, bit rate,
    preamble bytes, channel, MHz)"""
    if name in suite_nrf.NRF:
        mpm_tx, cc_setup = suite_nrf.nrf_setups(name, addr)
        rate = {"nrf1m": 1e6, "nrf250k": 250e3, "nrf2m": 2e6}[name]
        ch = 40 if ch is None else ch
        return mpm_tx, cc_setup, bytes.fromhex(addr)[::-1], 16, rate, 1, ch, 2400 + ch
    n, id_, rate_code, _ = suite_a7105.P[name]
    mpm_tx, _, cc_setup, _ = suite_a7105.setups(name, trim)
    ch = 170 if ch is None else ch
    return (mpm_tx, cc_setup, bytes.fromhex(id_ or PRESET_ID[name]), n, 500e3 / (rate_code + 1), 4, ch,
            a7105_mhz(ch))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("name", help=f"setup: {' '.join(list(suite_a7105.P) + list(suite_nrf.NRF))}")
    ap.add_argument("--ch", type=int, help="channel (default: A7105 170, NRF 40)")
    ap.add_argument("--trim", type=int, default=90)
    ap.add_argument("--addr", default="e7e7e7e7e7", help="NRF address, register order (default e7e7e7e7e7)")
    ap.add_argument("--count", type=int, default=8, help="packets per sender")
    ap.add_argument("--cc-pwr", default="default", help="CC2652 TX power word")
    ap.add_argument("--bits", nargs=2, type=int, metavar=("LO", "HI"), help="per-bit table")
    a = ap.parse_args()
    if a.name not in suite_a7105.P and a.name not in suite_nrf.NRF:
        ap.error(f"unknown setup {a.name}")
    mpm_tx, cc_setup, sync, n, rate, pre, ch, freq = setup(a.name, a.ch, a.trim, a.addr)
    payload = rig.pattern(n)
    label = "MPM NRF24L01" if a.name in suite_nrf.NRF else "MPM A7105"
    mpm, cc = rig.open_devices()
    with S.Sdr() as sdr, mpm, cc:
        sdr.cmd(f"FREQ {int(freq) - 1}")
        senders = ((label, mpm, rig.commands(mpm_tx)),
                   ("CC2652", cc, rig.commands(cc_setup) + [f"pwr {a.cc_pwr}"] + rig.commands(cc_setup)))
        for label, dev, setup_cmds in senders:
            rig.run(dev, setup_cmds + [f"ch {ch}"])
            dev.cmd(f"txp 1500 60000 {payload.hex()}")
            try:
                gain = sdr.autogain(min_len=int(0.9 * (pre + len(sync) + n + 2) * 8 / rate * S.FS))
                pk = packets(sdr, rate, sync, n, a.count, pre)
                summary(label, gain, pk, sync, payload, pre)
                if a.bits and pk:
                    bit_table(label, pk, sync, payload, pre, *a.bits)
            finally:
                dev.cmd("stop")
        cc.cmd("pwr default")
    return 0


if __name__ == "__main__":
    sys.exit(main())
