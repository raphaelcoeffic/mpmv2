#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5", "numpy", "logic2-automation"]
# ///
"""Bench `timing` on the CC2755 LaunchPad, measured by a Saleae Logic: the
board has no edge capture, its bench puts the RFE's PA enable on DIO17 and
the LNA enable on DIO18 during the test, with a 1 us pulse on DIO17 at each
run's start (bench_port.c). Logic 2 runs with its automation server
(`open -a Logic2 --args --automation`, port 10430).

LaunchPad: the solder bridges in front of the BoosterPack headers closed
(DIO17 = pin 32, DIO18 = pin 31). Defaults: DIO17 on channel 4, DIO18 on 5.

  timing_saleae.py                          FrSky X, 10 runs, lead 400 us
  timing_saleae.py --setup "preset nrf1m;ch 40" --lead 150
  timing_saleae.py --load capture_dir       analyze an export again

Per run, from its marker (about 1 us before the bench's t0), in us; the
cases in bench.c's order (11 markers a run count).
"""

import argparse
import csv
import os
import sys
import tempfile
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mpm_bench import Bench  # noqa: E402

CASES = ["tx", "hop_tx", "rx_tx", "rx_hop_tx", "rx_tx_at", "rx_hop_tx_at", "tx_at", "rx", "hop_rx",
         "tx_rx", "chain"]
TX_AT_LEAD_US = 1000  # bench.c's tx_at case
MARK_MAX_S = 5e-6


def capture(a, out: str):
    from saleae import automation

    with automation.Manager.connect(port=10430) as m:
        dev = automation.LogicDeviceConfiguration(enabled_digital_channels=[a.pa, a.lna],
                                                  digital_sample_rate=a.rate, digital_threshold_volts=3.3)
        cfg = automation.CaptureConfiguration(capture_mode=automation.TimedCaptureMode(duration_seconds=a.secs))
        with m.start_capture(device_configuration=dev, capture_configuration=cfg) as cap:
            time.sleep(0.3)
            with Bench(a.port, 921600) as b:
                b.cmd("stop")
                for c in a.setup.split(";"):
                    b.cmd(c.strip())
                b.cmd(f"timing {a.count} {a.len} {a.lead}", timeout=60)
            cap.wait()
            cap.export_raw_data_csv(directory=out, digital_channels=[a.pa, a.lna])


def load(path: str, pa: int, lna: int):
    rows = list(csv.reader(open(os.path.join(path, "digital.csv"))))
    head = rows[0]
    ipa, ilna = head.index(f"Channel {pa}"), head.index(f"Channel {lna}")
    t = np.array([float(r[0]) for r in rows[1:]])
    return t, np.array([int(r[ipa]) for r in rows[1:]]), np.array([int(r[ilna]) for r in rows[1:]])


def edges(t, c):
    return [(t[k], c[k]) for k in np.nonzero(np.diff(c))[0] + 1]


def after(es, level, t0):
    if t0 is None:
        return None
    return next((t for t, v in es if v == level and t > t0), None)


def analyze(t, cpa, clna, n: int, lead: float):
    epa, elna = edges(t, cpa), edges(t, clna)
    marks, pa = [], []
    k = 0
    while k < len(epa):
        if k + 1 < len(epa) and epa[k][1] == 1 and epa[k + 1][0] - epa[k][0] < MARK_MAX_S:
            marks.append(epa[k][0])
            k += 2
        else:
            pa.append(epa[k])
            k += 1
    print(f"{len(marks)} markers ({len(CASES)} x {n} expected), {len(pa)} PA edges, {len(elna)} LNA edges")
    res: dict[str, list[float]] = {}

    def add(name, a, b):
        if a is not None and b is not None:
            res.setdefault(name, []).append((b - a) * 1e6)

    for ci, case in enumerate(CASES):
        for m in marks[ci * n:(ci + 1) * n]:
            pr = after(pa, 1, m)
            pf = after(pa, 0, pr)
            lf, lr = after(elna, 0, m), after(elna, 1, m)
            if case in ("tx", "hop_tx"):
                add(f"{case}: call to PA on", m, pr)
            elif case in ("rx_tx", "rx_hop_tx"):
                add(f"{case}: call to LNA off", m, lf)
                add(f"{case}: call to PA on", m, pr)
            elif case in ("rx_tx_at", "rx_hop_tx_at"):
                add(f"{case}: PA on after the time given", m + lead * 1e-6, pr)
            elif case == "tx_at":
                add("tx_at: PA on after the time given", m + TX_AT_LEAD_US * 1e-6, pr)
            elif case == "rx":
                add("rx: call to LNA on", m, lr)
            elif case == "hop_rx":
                add("hop_rx: call to LNA off", m, lf)
                add("hop_rx: call to LNA on", m, after(elna, 1, lf))
            elif case in ("tx_rx", "chain"):
                add(f"{case}: PA off to LNA on", pf, after(elna, 1, pf))
            if case == "tx":
                add("tx: PA on (airtime + ramps)", pr, pf)
    for name, v in res.items():
        v = np.array(v)
        print(f"{name:42} n={len(v):3} min={v.min():7.1f} avg={v.mean():7.1f} max={v.max():7.1f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=os.environ.get("CC27_PORT"), help="CC2755 bench (default $CC27_PORT)")
    ap.add_argument("--setup", default="preset frskyx;ch 0")
    ap.add_argument("--count", type=int, default=10, help="runs per case")
    ap.add_argument("--len", type=int, default=16, help="payload bytes")
    ap.add_argument("--lead", type=int, default=400, help="TX at a time: lead, us")
    ap.add_argument("--pa", type=int, default=4, help="Saleae channel on DIO17")
    ap.add_argument("--lna", type=int, default=5, help="Saleae channel on DIO18")
    ap.add_argument("--rate", type=int, default=25_000_000, help="sample rate, Hz")
    ap.add_argument("--secs", type=float, default=6, help="capture length, s")
    ap.add_argument("--out", help="keep the export in this directory")
    ap.add_argument("--load", help="analyze an export (directory with digital.csv)")
    a = ap.parse_args()
    path = a.load
    if not path and not a.port:
        raise SystemExit("CC2755 port: --port or $CC27_PORT")
    if not path:
        path = a.out or tempfile.mkdtemp(prefix="timing_saleae")
        capture(a, path)
    analyze(*load(path, a.pa, a.lna), a.count, a.lead)


if __name__ == "__main__":
    main()
