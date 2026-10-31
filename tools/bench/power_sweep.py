#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""TX power curves: one board sends at each of its power levels, another
receives (RSSI in dBm, the median of each level on 4 channels, then the mean
of the channels). Steps below full power are what counts: absolute levels
depend on where the boards stand and how their antennas point.

  power_sweep.py v2          MPM v2 sends (RF core words), LaunchPad receives
  power_sweep.py lp          LaunchPad sends (RF core words), MPM v2 receives
  power_sweep.py nrf|cc2500|a7105|cyrf
                             the 4-in-1's chip sends (its power register),
                             LaunchPad receives
  power_sweep.py recip       MPM v2 output power from the path both ways
                             (LaunchPad <-> MPM v2): the path cancels

recip: the LaunchPad sends at 0x7217 (+5 dBm, TI's table), the MPM v2 at
--word. MPM v2 out = RSSI at the LaunchPad - RSSI at the MPM v2 + 5 + the MPM
v2's RX gain (the SKY66112's 11 dB LNA - about 3 dB of balun, switch and
filter: 8 dB, --rx-gain).

The LaunchPad's RSSI compresses about 2 dB above -17 dBm: keep the strongest
level below it (distance). The MPM v2's antenna: --ant 1 (J2) or 2 (J3).
Ports: $CC_PORT (LaunchPad bench), $V2_PORT (MPM v2 bench), $MPM_PORT
(4-in-1 bench). See docs/interop-results.md, TX power.
"""

import argparse
import statistics
import time

import rig
from mpm_bench import Bench, find_port

WORDS = [0x04C1, 0x04C2, 0x04C3, 0x0481, 0x04C4, 0x0482, 0x0441, 0x04C5, 0x04C6, 0x0484,
         0x0443, 0x06C8, 0x06CA, 0x0ACC, 0x0ACF, 0x0AD0, 0x10D4, 0x12D6, 0x1893, 0x2853,
         0x2856, 0x3259, 0x385D, 0x4E63, 0x7217]
NRF = [0x01, 0x03, 0x05, 0x07]  # RF_SETUP: RF_PWR 0 to 3, LNA_HCURR
CC2500 = [0x50, 0x44, 0xC0, 0x84, 0x81, 0x46, 0x93, 0x55, 0x8D, 0xC6, 0x97, 0x6E, 0x7F, 0xA9,
          0xBB, 0xFE, 0xFF]  # PATABLE: CC2500_POWER_1 to 17
A7105 = [0x00, 0x01, 0x02, 0x04, 0x0D, 0x17, 0x1F]  # TX_TEST: A7105_POWER_0 to 6
CYRF = list(range(8))  # TX_CFG PA_SETTING
NRF_SETUP = ("preset nrf1m;nrf c3c3c3c3c3 2", "cc2652_nrf_1m.txt", 16)
CHANS = [20, 45, 65, 78]

# name: sender, its setup, receiver setup, payload length, channels, levels
CASES = {
    "v2": ("v2", *NRF_SETUP, CHANS, WORDS),
    "lp": ("lp", *NRF_SETUP, CHANS, WORDS),
    "nrf": ("mpm", "nrf_1m.txt", "cc2652_nrf_1m.txt", 16, CHANS, NRF),
    "cc2500": ("mpm", "frskyd.txt", "cc2652_frskyd.txt", 18, [20, 80, 140, 200], CC2500),
    "a7105": ("mpm", "afhds2a.txt", "cc2652_afhds2a.txt", 38, [40, 90, 130, 156], A7105),
    "cyrf": ("mpm", "dsm.txt", "cc2652_dsm.txt", 16, CHANS, CYRF),
}


def antenna(v2: Bench, ant: int):
    """MPM v2 ANT_SEL (DIO15): GPIO DOUTSET / DOUTCLR"""
    v2.cmd(f"poke {0x40022090 if ant == 2 else 0x400220A0:#x} 0x8000")


def rssi(tx: Bench, rx: Bench, payload: bytes, n: int, period: int, bad_ok: bool = False):
    rig.drain(rx)
    rx.cmd(f"rx {len(payload)}")
    tx.cmd(f"txp {period} {n} {payload.hex()}")
    time.sleep(n * period / 1e6 + 0.15)
    rx.cmd("stop")
    tx.cmd("stop")
    time.sleep(0.05)
    rs = [int(r) for e in rig.events(rx) if bad_ok or "crc=bad" not in e
          if (r := rig.flag(e, "rssi")) is not None]
    return statistics.median(rs) if rs else None


def sweep(a):
    dev, tx_setup, rx_setup, ln, chans, levels = CASES[a.case]
    lp = Bench(find_port("cc"), 921600)
    if dev == "mpm":
        tx, rx = Bench(find_port("mpm")), lp
    else:
        v2 = Bench(find_port("v2"), 921600)
        rig.run(v2, "chip cc2652")
        antenna(v2, a.ant)
        tx, rx = (v2, lp) if dev == "v2" else (lp, v2)
    rig.run(rx, "chip cc2652")
    # CC2500: the length byte first; its bench sends a few bytes wrong (CRC
    # bad), the sync word and the RSSI are right
    payload = bytes([ln - 1]) + bytes(range(1, ln)) if a.case == "cc2500" else bytes(range(ln))
    res = {lv: [] for lv in levels}
    for ch in chans:
        rig.run(tx, tx_setup)
        rig.run(rx, rx_setup)
        rig.run(tx, f"ch {ch}" + (";s 0x33" if a.case == "cc2500" else ""))  # SCAL
        rig.run(rx, f"ch {ch}")
        cfg = int(tx.cmd("r 0x03").split()[-1], 16) & ~7 if a.case == "cyrf" else 0
        for lv in levels:
            cmd = {"nrf": f"w 0x06 {lv:02x}", "cc2500": f"w 0x3e {lv:02x}",
                   "a7105": f"w 0x28 {lv:02x}", "cyrf": f"w 0x03 {cfg | lv:02x}"}.get(a.case, f"pwr {lv:#x}")
            tx.cmd(cmd)
            med = rssi(tx, rx, payload, a.n, a.period, a.case == "cc2500")
            res[lv].append(med)
            print(f"ch {ch:3d} level {lv:#06x}: rssi {med}", flush=True)
    means = {lv: sum(v) / len(v) for lv, v in res.items() if None not in v}
    top = means.get(max(levels), max(means.values()) if means else 0)
    print(f"\n{a.case}: level, rssi per channel {chans}, mean, below the top level")
    for lv in levels:
        m = means.get(lv)
        print(f"  {lv:#06x}  {res[lv]}  " + (f"{m:.1f}  {top - m:.1f}" if m is not None else "-"))


def recip(a):
    lp, v2 = Bench(find_port("cc"), 921600), Bench(find_port("v2"), 921600)
    for d in (lp, v2):
        rig.run(d, "chip cc2652")
    antenna(v2, a.ant)
    lp.cmd("pwr 0x7217")
    v2.cmd(f"pwr {a.word:#x}")
    payload = bytes(range(16))
    diffs = []
    for ch in (20, 32, 45, 58, 65, 78):
        for d in (lp, v2):
            rig.run(d, f"{NRF_SETUP[0]};ch {ch}")
        at_lp = rssi(v2, lp, payload, a.n, a.period)
        at_v2 = rssi(lp, v2, payload, a.n, a.period)
        print(f"ch {ch}: at the LaunchPad {at_lp}, at the MPM v2 {at_v2}", flush=True)
        if at_lp is not None and at_v2 is not None:
            diffs.append(at_lp - at_v2)
    m = sum(diffs) / len(diffs)
    print(f"MPM v2 {a.word:#06x} out: {m:.1f} + 5 + {a.rx_gain} = {m + 5 + a.rx_gain:.1f} dBm")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("case", choices=[*CASES, "recip"])
    ap.add_argument("--n", type=int, default=40, help="packets per level and channel")
    ap.add_argument("--period", type=int, default=4000, help="us")
    ap.add_argument("--ant", type=int, default=1, choices=(1, 2), help="MPM v2 antenna")
    ap.add_argument("--word", type=lambda s: int(s, 0), default=0x2853, help="recip: MPM v2 word")
    ap.add_argument("--rx-gain", type=float, default=8.0, help="recip: MPM v2 RX gain, dB")
    a = ap.parse_args()
    recip(a) if a.case == "recip" else sweep(a)


if __name__ == "__main__":
    main()
