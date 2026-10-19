#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""Automatic frequency trim: the CC2652 learns the MPM chip's frequency from its
packets (autotrim, the modem's estimate), then sends to it.

  autotrim.py a7105 wfly2 fixed90 0 35        CC2652 -> A7105 at trim 90, then
                                              autotrim offsets 0 and +35 kHz
  autotrim.py cc2500 frskyx fixed-35 -20 --start 40
  autotrim.py a7105 flysky --sweep -40 60 160  estimate (@rx foff=) per trim

Each autotrim run starts at --start, the MPM sends --learn packets (autotrim
steps print as @trim), then the CC2652 sends --count packets to the MPM at the
trim found. fixed<N>: plain trim N, no learning. --sweep: the MPM sends at
each trim, median and spread of the estimate, its slope against the trim
(about -1: the estimate's scale) and the trim where it is 0.

Families: a7105, cc2500, cyrf (suite_<family>.py setups), nrf (suite_nrf.py
plain NRF24L01 setups: nrf1m, nrf250k, nrf2m; --cc-extra "preset nrf2mble").
"""

import argparse
import statistics
import sys
import time

import rig
import suite_a7105
import suite_cc2500
import suite_cyrf
import suite_nrf


def setups(mpm, a):
    """(MPM TX setup, MPM RX setup, CC2652 setup, payload, period_us, cc2500), trims removed."""
    cc2500 = a.family == "cc2500"
    period = 5000
    if cc2500:
        m, c, n = suite_cc2500.P[a.name]
        mtx = mrx = rig.commands(m)
        rig.run(mpm, mtx)
        p = suite_cc2500.phy(bytes.fromhex(mpm.cmd("r 0x00 0x2f")))
        n = n or (min(p["pktlen"], 40) if p["var"] else p["pktlen"])
        payload = rig.pattern(n)
        if p["var"]:
            payload = bytes([n - 1]) + payload[1:]
        airtime = (n + p["pre"] + 6) * 8 * (2 if p["fec"] else 1) / p["rate"] * 1e6
        period = max(3000, int(airtime) + 2000)
        cc = rig.commands(c if "ch " in c else c + ";ch 0")
    elif a.family == "cyrf":
        m, c, n, m_rx, c_tx = (suite_cyrf.P[a.name] + ("", ""))[:5]
        mtx, mrx = rig.commands(m), rig.commands(m) + rig.commands(m_rx)
        cc = rig.commands(c) + rig.commands(c_tx)
        payload = rig.pattern(n)
    elif a.family == "nrf":
        m, c = suite_nrf.nrf_setups(a.name, "e7e7e7e7e7")
        mtx = mrx = rig.commands(m) + ["ch 40"]
        cc = rig.commands(c) + ["ch 40"]
        payload = rig.pattern(16)
    else:
        m_tx, m_rx, c, n = suite_a7105.setups(a.name)
        mtx, mrx, cc = (rig.commands(s) + ["ch 170"] for s in (m_tx, m_rx, c))
        payload = rig.pattern(n)
    cc = [x for x in cc if not x.startswith("trim")] + rig.commands(a.cc_extra)
    return mtx, mrx, cc, payload, period, cc2500


def receive(mpm, cc, mtx, payload, period, count) -> list[str]:
    rig.run(mpm, mtx)
    rig.drain(cc)
    cc.cmd(f"rx {len(payload)}")
    mpm.cmd(f"txp {period} {count} {payload.hex()}")
    mpm.wait_event("@ev txdone", count * period / 1e6 + 2)
    time.sleep(0.1)
    ev = rig.events(cc, "@")
    cc.cmd("stop")
    mpm.cmd("stop")
    return ev


def sweep(mpm, cc, s, a):
    mtx, _, cc_setup, payload, period, _ = s
    res = []
    for t in a.modes:
        rig.run(cc, ["autotrim off", f"trim {t}"] + cc_setup)
        ev = receive(mpm, cc, mtx, payload, period, a.learn)
        f = [int(rig.flag(e, "foff")) for e in ev
             if e.startswith("@rx") and rig.flag(e, "foff") and rig.flag(e, "crc") != "bad"]
        if not f:
            print(f"trim {t:+5d}: no estimate ({len(ev)} packets)", flush=True)
            continue
        res.append((t, statistics.median(f)))
        print(f"trim {t:+5d}: {len(f):4d} estimates, median {statistics.median(f):+6.1f} kHz, "
              f"sd {statistics.pstdev(f):4.1f}", flush=True)
    if len(res) >= 2:
        xs, ys = [r[0] for r in res], [r[1] for r in res]
        mx, my = statistics.mean(xs), statistics.mean(ys)
        sl = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
        print(f"slope {sl:+.2f}, 0 at trim {mx - my / sl:+.0f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("family", choices=("a7105", "cc2500", "cyrf", "nrf"))
    ap.add_argument("name", help="setup of the family's suite")
    ap.add_argument("modes", nargs="+", help="autotrim offsets in kHz or fixed<trim>; --sweep: trims")
    ap.add_argument("--start", type=int, default=0, help="trim before learning (default 0)")
    ap.add_argument("--learn", type=int, default=48, help="packets to learn from (default 48)")
    ap.add_argument("--count", type=int, default=250, help="packets CC2652 -> MPM (default 250)")
    ap.add_argument("--rounds", type=int, default=1, help="repeat the modes (default 1)")
    ap.add_argument("--sweep", action="store_true", help="estimate per trim")
    ap.add_argument("--cc-extra", default="", help="CC2652 commands after its setup")
    a = ap.parse_args()
    mpm, cc = rig.open_devices()
    with mpm, cc:
        s = setups(mpm, a)
        if a.sweep:
            a.modes = [int(t) for t in a.modes]
            sweep(mpm, cc, s, a)
            return 0
        mtx, mrx, cc_setup, payload, period, cc2500 = s
        for _ in range(a.rounds):
            for mode in a.modes:
                steps = ""
                if mode.startswith("fixed"):
                    rig.run(cc, ["autotrim off", f"trim {int(mode[5:])}"] + cc_setup)
                else:
                    rig.run(cc, ["autotrim off", f"trim {a.start}"] + cc_setup + [f"autotrim {mode}"])
                    ev = receive(mpm, cc, mtx, payload, period, a.learn)
                    steps = " ".join(e.split()[1] for e in ev if e.startswith("@trim"))
                    rig.run(cc, "autotrim off")
                r = rig.link_test(mpm, cc, payload, mpm_setup=mrx, cc_setup=cc_setup, period=period,
                                  count=a.count, direction="rev", cc2500=cc2500)["rev"]
                trim = dict(x.split("=", 1) for x in cc.cmd("info").split() if "=" in x)["trim"]
                print(f"{a.name} {mode:>8}: {rig.fmt(r)}  trim {trim}"
                      + (f" (steps {steps})" if steps else ""), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
