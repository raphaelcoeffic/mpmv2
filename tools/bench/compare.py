#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""Two radio boards against the MPM's chips, in the same run: the suites'
tests (suite_cc2500.py, suite_a7105.py, suite_cyrf.py, suite_nrf.py) on each
board in turn (A B, then B A on the next test: drift and Wi-Fi alike), both
ways. "rev" is the board sending (protocol packets), "fwd" the MPM's chip
sending (telemetry).

  compare.py sweep cc2500 a7105 --out sweep.json     trim windows
  compare.py run --trims sweep.json --out run.json   full counts, each board
                                                     at its window centres
  compare.py report sweep.json run.json              tables (Markdown)

Boards (both on 921600 baud UARTs): v2 = the MPM v2 ($V2_PORT, CC2652RB and
its SKY66112, default word 0x2853: about +20 dBm out), cc27 = the CC2755P10
LaunchPad ($CC_PORT) at +5 dBm: the level of the MPM v2 at the 4-in-1 (on
the SDR board: at +20 dBm the LaunchPad arrives about 15 dB stronger, enough
to overload the A7105 at 500 kbps). coronav2 lowers the power (the CC2500's
frozen AGC takes a window of levels): the suite's word on the MPM v2, -10 dBm
on the CC27.

sweep: each test at trim offsets (--offsets, kHz) around the suites' trims,
fewer packets (--count, default 50). run: the suites' counts, each test at the
centre of each board's window from --trims (else the suites' trims).
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import sys

import rig
import suite_a7105
import suite_cc2500
import suite_cyrf
import suite_nrf
from mpm_bench import Bench, find_port

BOARDS = {
    # name: (port kind, power word, power for the tests with a lower one)
    "v2": ("v2", "0x2853", {"0x10c3": "0x10c3"}),
    # coronav2 lowers the power against the CC2500's frozen AGC, which takes a
    # window of levels: the CC27 at -10 dBm (2449 MHz: 296/300, the MPM v2 at
    # 0x10c3 295/300; at -20 dBm 80 to 86%, bit errors; at 0 dBm 0%)
    "cc27": ("cc", "5", {"0x10c3": "0xf6"}),
}
FAMILIES = ("cc2500", "a7105", "cyrf", "nrf")
OK = 0.95  # a window: the offsets where at least 95% arrive


NAMES = {"cc2500": list(suite_cc2500.P), "a7105": list(suite_a7105.P), "cyrf": list(suite_cyrf.P),
         "nrf": list(suite_nrf.NRF) + list(suite_nrf.P)}


def cases(mpm: Bench, family: str, names: list[str] | None, count: int | None) -> list[rig.Case]:
    if names:
        names = [n for n in names if n in NAMES[family]]
        if not names:
            return []
    if family == "cc2500":
        cs = list(suite_cc2500.cases(mpm, names))
    elif family == "a7105":
        cs = list(suite_a7105.cases(names))
    elif family == "cyrf":
        cs = list(suite_cyrf.cases(names))
    else:
        cs = list(suite_nrf.cases(names))
    if count:
        cs = [dataclasses.replace(c, kw=dict(c.kw, count=count)) for c in cs]
    return cs


def run_board(mpm: Bench, dev: Bench, board: str, case: rig.Case, trim: int, verbose: bool) -> dict:
    _, pwr, low = BOARDS[board]
    if case.pwr:
        case = dataclasses.replace(case, pwr=low[case.pwr])
    return rig.run_case(mpm, dev, case, trim=trim, pwr=pwr, verbose=verbose)


def rate(r: dict) -> float:
    return r["good"] / r["count"]


def windows(points: dict) -> dict:
    """{offset: {dir: result}} -> per direction the offsets passing, and the
    centre of the longest run of offsets where every direction passes."""
    offs = sorted(points, key=int)
    dirs = sorted({d for p in points.values() for d in p})
    out: dict = {d: [int(o) for o in offs if d in points[o] and rate(points[o][d]) >= OK] for d in dirs}
    best, run = [], []
    for o in offs:
        if all(d in points[o] and rate(points[o][d]) >= OK for d in dirs):
            run.append(int(o))
            if len(run) > len(best):
                best = list(run)
        else:
            run = []
    if not best:  # nowhere both: the best sum
        o = max(offs, key=lambda o: sum(rate(points[o][d]) for d in points[o]))
        best = [int(o)]
    out["centre"] = (best[0] + best[-1]) // 2
    return out


def open_boards(names: list[str]) -> dict[str, Bench]:
    devs = {}
    for b in names:
        devs[b] = Bench(find_port(BOARDS[b][0]), 921600)
    return devs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=("sweep", "run", "report"))
    ap.add_argument("args", nargs="*", help="families (sweep, run: default all) or JSON files (report)")
    ap.add_argument("--names", nargs="*", help="only these tests")
    ap.add_argument("--boards", nargs="*", default=list(BOARDS), help="boards (default: v2 cc27)")
    ap.add_argument("--offsets", nargs="*", type=int,
                    default=[-150, -125, -100, -75, -50, -25, 0, 25, 50, 75, 100])
    ap.add_argument("--count", type=int, help="packets per test (sweep: 50, run: the suite's)")
    ap.add_argument("--trims", help="sweep JSON: each board at its window centres (run)")
    ap.add_argument("--out", help="results JSON (merged into an existing file)")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    if a.mode == "report":
        return report(a.args)
    families = a.args or list(FAMILIES)
    count = a.count or (50 if a.mode == "sweep" else None)
    centres = {}
    if a.trims:
        with open(a.trims) as f:
            for fam, tests in json.load(f).items():
                merged = {}  # (board) -> {offset: {test dir: result}}: the family's centre
                for name, boards in tests.items():
                    for b, pts in boards.items():
                        centres[(fam, name, b)] = windows(pts)["centre"]
                        for o, p in pts.items():
                            merged.setdefault(b, {}).setdefault(o, {}).update(
                                {f"{name} {d}": v for d, v in p.items()})
                for b, pts in merged.items():
                    # only the offsets every test ran at
                    n = max(len(p) for p in pts.values())
                    centres[(fam, None, b)] = windows({o: p for o, p in pts.items() if len(p) == n})["centre"]
    out = {}
    if a.out:
        try:
            with open(a.out) as f:
                out = json.load(f)
        except FileNotFoundError:
            pass
    mpm = Bench(find_port("mpm"))
    devs = open_boards(a.boards)
    with mpm:
        mpm.cmd("info")  # resets the chips (NRF24L01 CRC trap after a restart)
        for d in devs.values():
            d.__enter__()
            rig.run(d, "stop;chip cc2652")  # stop: a job left running by an interrupted run
        flip = False
        try:
            for fam in families:
                for case in cases(mpm, fam, a.names, count):
                    order = list(a.boards)[::-1] if flip else list(a.boards)
                    flip = not flip
                    offsets = a.offsets if a.mode == "sweep" else [None]
                    for off in offsets:
                        for b in order:
                            if off is None:
                                # the test's window centre, else its family's
                                centre = centres.get((fam, case.name, b), centres.get((fam, None, b), 0))
                                trim = case.trim + centre
                                key = "run"
                            else:
                                trim, key = case.trim + off, str(off)
                            r = run_board(mpm, devs[b], b, case, trim, a.verbose)
                            slot = out.setdefault(fam, {}).setdefault(case.name, {}).setdefault(b, {})
                            slot.setdefault(key, {}).update(r)
                            if a.mode == "run":
                                slot[key]["trim"] = trim
                            line = " ".join(f"{d} {100 * rate(v):5.1f}%" for d, v in r.items())
                            print(f"{fam:6s} {case.name:24s} {b:4s} trim {trim:+5d}  {line}", flush=True)
                    if a.out:
                        with open(a.out, "w") as f:
                            json.dump(out, f, indent=1)
        finally:
            for d in devs.values():
                d.cmd("stop")
                d.__exit__(None, None, None)
    return 0


def report(files: list[str]) -> int:
    for path in files:
        with open(path) as f:
            data = json.load(f)
        for fam, tests in data.items():
            boards = sorted({b for t in tests.values() for b in t})
            sweep = not any("run" in pts for t in tests.values() for pts in t.values())
            print(f"\n### {fam} ({'trim windows, offsets passing >= 95%' if sweep else 'pass rates'})\n")
            if sweep:
                print("| Test | " + " | ".join(f"{b} fwd | {b} rev | {b} centre" for b in boards) + " |")
                print("|---|" + "---|---|---|" * len(boards))
            else:
                print("| Test | " + " | ".join(f"{b} fwd | {b} rev" for b in boards) + " |")
                print("|---|" + "---|---|" * len(boards))
            for name, t in tests.items():
                cells = []
                for b in boards:
                    pts = t.get(b, {})
                    if sweep:
                        w = windows(pts) if pts else {}

                        def span(d):
                            o = w.get(d)
                            return "none" if o == [] else "" if o is None else f"{o[0]:+d}..{o[-1]:+d}"
                        cells += [span("fwd"), span("rev"), f"{w['centre']:+d}" if w else ""]
                    else:
                        r = pts.get("run", {})
                        cells += [f"{100 * rate(r[d]):.1f}%" if d in r else "" for d in ("fwd", "rev")]
                print(f"| {name} | " + " | ".join(cells) + " |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
