#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""RF core latencies of the CC2652 (bench `timing`), per PHY: average (and
maximum) in us, from the PA and LNA enable edges of the RF core.

  timing.py                       all setups, LaunchPad
  CC_PORT=v2 timing.py nrf1m      MPM v2 (TX only: no LNA enable pin there)
  timing.py frskyx --count 100 --edges

Rows (t0 = the call):
  init          radio_init(): PHY setup, synthesizer included
  fs, fs_same   radio_set_frequency() while idle: 20 MHz hops, same frequency
  tx_call       radio_tx_start() returns
  tx_start      t0 to the PA enable
  tx_pa         PA enable length (compare with the airtime)
  tx_end        end of the PA enable to radio_tx_wait() returning
  hop_tx        radio_set_frequency() + radio_tx_start() to the PA enable
  rx_tx         RX running, radio_tx_start() to the PA enable
  tx_at         radio_tx_start_at() 1 ms ahead: PA enable from that time
  rx_call       radio_rx_start() returns
  rx_stop       radio_rx_stop() returns
  rx_on, rx_off radio_rx_start() / radio_rx_stop() to the LNA enable edge
  hop_rx        RX running, radio_set_frequency() + radio_rx_start() to RX on
  tx_rx         radio_tx_wait() + radio_rx_start(): end of PA to RX on
  chain_tx_rx   radio_send_rx() (chained commands): end of PA to RX on
  chain_window  its 200 us RX window: LNA enable length
  rssi_valid    radio_rx_start() to the first valid radio_rssi()
  rssi_call     one radio_rssi() call
  rssi_dbm      the first valid values (min, max)
"""

import argparse
import queue
import sys

import mpm_bench
import rig

# name: (CC2652 setup, payload length as the protocol sends it)
SETUPS = {
    "nrf1m": ("preset nrf1m;nrf e7e7e7e7e7 2", 16),
    "nrf250k": ("preset nrf250k;nrf e7e7e7e7e7 2", 16),
    "nrf2mble": ("preset nrf2mble;nrf e7e7e7e7e7 2", 32),
    "frskyd": ("preset frskyd", 18),
    "frskyx": ("preset frskyx", 30),
    "afhds2a": ("preset afhds2a", 37),
    "dsm": ("preset dsm", 16),
}


def measure(cc: mpm_bench.Bench, setup: str, count: int, length: int) -> tuple[dict, list[str]]:
    rig.run(cc, setup + ";ch 40")
    while not cc.events.empty():
        cc.events.get_nowait()
    cc.cmd(f"timing {count} {length}", timeout=60)
    rows, edges = {}, []
    while True:
        try:
            ev = cc.events.get(timeout=0.3)
        except queue.Empty:
            break
        if ev.startswith("@edges"):
            edges.append(ev[7:])
        elif ev.startswith("@timing"):
            name, *fields = ev.split()[1:]
            if "=" in name:  # airtime_us= air_bytes= payload=
                fields, name = [name] + fields, "airtime"
            rows[name] = dict(f.split("=", 1) for f in fields)
    return rows, edges


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", help=f"setups (default: all): {' '.join(SETUPS)}")
    ap.add_argument("--count", type=int, default=20, help="runs per measurement (default 20)")
    ap.add_argument("--edges", action="store_true", help="print the edges of the first runs")
    a = ap.parse_args()
    names = a.names or list(SETUPS)
    for n in names:
        if n not in SETUPS:
            ap.error(f"unknown setup {n}")
    cc = mpm_bench.Bench(mpm_bench.find_port("cc"), 921600)
    results = {}
    with cc:
        for n in names:
            setup, length = SETUPS[n]
            results[n], edges = measure(cc, setup, a.count, length)
            if a.edges:
                for e in edges:
                    print(f"{n}: {e}")
    order = []
    for r in results.values():
        order += [k for k in r if k not in order]
    print(f"{'us':14s}" + "".join(f"{n:>17s}" for n in names))
    for k in order:
        cells = []
        for n in names:
            r = results[n].get(k)
            if not r:
                cells.append("")
            elif k == "airtime":
                cells.append(f"{r['airtime_us']} ({r['air_bytes']} B)")
            elif k == "rssi_dbm":
                cells.append(f"{r['min']} / {r['max']}")
            elif "avg" in r:
                cells.append(f"{float(r['avg']):.0f} ({float(r['max']):.0f})")
            else:
                cells.append("missed")
        print(f"{k:14s}" + "".join(f"{c:>17s}" for c in cells))
    return 0


if __name__ == "__main__":
    sys.exit(main())
