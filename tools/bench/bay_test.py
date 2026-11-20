#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""Module bay lines without an inverter or RX mux on the LP-EM-CC2755P10 (cc27-bench,
the "bay" commands of src/boards/cc2755p10_lp/bench_bay.c), driven by
baysim built for GP4/GP5 (baysim/, LINK_UART=1 LINK_TX_PIN=4
LINK_RX_PIN=5): GP4 -> DIO22 (the radio's line), GP5 <- DIO21 (S.Port, open
drain), GND. No inverter, no RX mux: the pad modes do it.

  bay_test.py echo     UART1 echo, both line polarities on each side, at
                       100000 8E2 (MULTI) and 57600 8N1 (STK500). Expected
                       to pass when the LaunchPad's input inversion matches
                       the line, to fail otherwise
  bay_test.py edges    PPM from baysim, timed from the pad's edge detection
                       while UART1 owns DIO22
  bay_test.py u0       UART0 RX moved to DIO20 (needs GP4 on DIO20 too):
                       baysim's bytes in, the XDS110's left out
  bay_test.py all      the three

Ports: --port (the bench, 921600), else $CC_PORT, else the first XDS110;
baysim found by its USB IDs.
"""
from __future__ import annotations

import argparse
import os
import random
import sys
import threading
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).parent))
from baysim import BaySim, find_ports  # noqa: E402
from mpm_bench import Bench, BenchError, find_port  # noqa: E402


def bench_cmd_async(bench: Bench, line: str, timeout: float):
    """bench.cmd() in a thread: the commands block while baysim drives the line"""
    res: dict = {}

    def run():
        try:
            res["ok"] = bench.cmd(line, timeout)
        except BenchError as e:
            res["err"] = str(e)

    t = threading.Thread(target=run)
    t.start()
    return t, res


def open_link(port: str, baud: int, mode: str) -> serial.Serial:
    parity = serial.PARITY_EVEN if mode == "8e2" else serial.PARITY_NONE
    stop = serial.STOPBITS_TWO if mode == "8e2" else serial.STOPBITS_ONE
    return serial.Serial(port, baud, parity=parity, stopbits=stop, timeout=0.05)


def echo_case(bench, bay, link_port, baud, mode, radio_inv, lp_inv, txmode, pu):
    sport_inv = 1 if txmode in ("odinv", "ppinv") else 0
    bay.cmd(f"invert {radio_inv} {sport_inv}")
    data = bytes(random.randrange(256) for _ in range(200))
    with open_link(link_port, baud, mode) as link:
        time.sleep(0.05)
        link.reset_input_buffer()
        t, res = bench_cmd_async(bench, f"bay echo {baud} {mode} {lp_inv} {txmode} {pu} 600", 3)
        time.sleep(0.1)
        got = b""
        for i in range(0, len(data), 20):
            link.write(data[i:i + 20])
            link.flush()
            time.sleep(0.01)
        end = time.monotonic() + 0.3
        while time.monotonic() < end:
            got += link.read(256)
        t.join()
    same = sum(a == b for a, b in zip(data, got))
    ok = got == data
    return ok, f"sent={len(data)} back={len(got)} same={same} lp: {res.get('ok', res.get('err'))}"


def cmd_echo(bench, bay, link_port, pu):
    fails = 0
    for baud, mode in ((100000, "8e2"), (57600, "8n1")):
        for txmode in ("od", "odinv"):
            for radio_inv in (0, 1):
                for lp_inv in (0, 1):
                    expect = radio_inv == lp_inv
                    ok, info = echo_case(bench, bay, link_port, baud, mode, radio_inv, lp_inv, txmode, pu)
                    verdict = "as expected" if ok == expect else "UNEXPECTED"
                    fails += ok != expect
                    print(f"{baud:6} {mode} line={'low' if radio_inv else 'high'}-idle "
                          f"lp_rxinv={lp_inv} sport={txmode:5}: {'PASS' if ok else 'fail'} "
                          f"({verdict}) {info}")
    bay.cmd("invert 0 1")
    return fails


def cmd_edges(bench, bay):
    chans = [1000, 1250, 1500, 1750, 2000, 1100, 1900, 1500]
    bay.cmd("invert 0 1")
    print("baysim:", bay.cmd("ppm - 22500 300 " + " ".join(map(str, chans))))
    time.sleep(0.1)
    while not bench.events.empty():
        bench.events.get_nowait()
    try:
        r = bench.cmd("bay edges 0 120", 3)
    finally:
        print("baysim:", bay.cmd("ppm off"))
    edges = []
    while not bench.events.empty():
        a = bench.events.get_nowait().split()
        if a[:2] == ["@bay", "e"]:
            edges.append((int(a[2]), int(a[3])))
    print("lp:", r)
    # pulses low: a falling edge (level 0 after it) starts each channel
    falls = [t for t, lvl in edges if lvl == 0]
    gaps = [b - a for a, b in zip(falls, falls[1:])]
    # the sync gap is the long one: channels follow it
    frames, cur = [], None
    for g in gaps:
        if g > 3000:
            if cur:
                frames.append(cur)
            cur = []
        elif cur is not None:
            cur.append(g)
    widths = [b - a for (a, la), (b, lb) in zip(edges, edges[1:]) if la == 0 and lb == 1]
    print(f"edges={len(edges)} pulse widths (us): {sorted(set(widths))}")
    bad = 0
    for f in frames:
        f = f[:len(chans)]
        err = max(abs(x - c) for x, c in zip(f, chans)) if len(f) == len(chans) else None
        bad += err is None or err > 3
        print(f"frame: {f} max error {err} us")
    if not frames:
        print("no whole frame")
    return 0 if frames and not bad else 1


def cmd_u0(bench, bay, link_port):
    with open_link(link_port, 921600, "8n1") as link:
        bay.cmd("invert 0 1")
        t, res = bench_cmd_async(bench, "bay u0 800", 3)
        time.sleep(0.2)
        link.write(b"PICO")
        bench.ser.write(b"XDS")
        t.join()
    r = res.get("ok", res.get("err", ""))
    print("lp:", r)
    hexs = r.split("hex=")[-1] if "hex=" in r else ""
    if hexs == b"PICO".hex():
        print("u0: baysim's bytes on DIO20, none from the XDS110: PASS")
        return 0
    if hexs == "":
        print("u0: nothing received (DIO20 not wired?); the XDS110's bytes left out")
        return 1
    print(f"u0: got {bytes.fromhex(hexs)!r}: fail")
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("test", choices=("echo", "edges", "u0", "all"))
    ap.add_argument("-p", "--port", default=os.environ.get("CC_PORT"))
    ap.add_argument("--pu", type=int, default=1, help="pad pull-up on DIO21 (default 1: no external pull-up on the LaunchPad)")
    a = ap.parse_args()

    link_port, ctrl_port = find_ports()
    bay = BaySim(ctrl_port)
    bench = Bench(a.port or find_port("cc"), 921600)
    fails = 0
    try:
        print("baysim:", bay.cmd("ping"), "| lp:", bench.cmd("info")[:60])
        bay.cmd("tx drive")
        if a.test in ("echo", "all"):
            fails += cmd_echo(bench, bay, link_port, a.pu)
        if a.test in ("edges", "all"):
            fails += cmd_edges(bench, bay)
        if a.test in ("u0", "all"):
            fails += cmd_u0(bench, bay, link_port)
    finally:
        bench.close()
        bay.close()
    print("FAILURES:", fails)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
