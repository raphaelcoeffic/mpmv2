#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""Where the lost packets of a suite setup go: missing or bad, which bytes and
bits are wrong, the RSSI, the receiver counters and the timing of the losses.

  losses.py cyrf devo                          MPM -> CC2652 (fwd), ch 30
  losses.py cyrf devo --mpm-extra "w 0x03 0x0f" --ch 8 --runs 3
  losses.py a7105 joysway --dir rev --payload 3ce0d55691cc07427db8f32e69a4df1a
  losses.py a7105 wfly2 --dir rev --cc-extra "pwr 0x04c6" -q
  losses.py nrf nrf2m --dir rev --addr 6565656565
  losses.py nrf nrf2m --cc-extra "chip sx1281"   NRF -> MPM v2 SX1281, 2 Mbps
  losses.py nrf nrf2mble                       NRF -> CC2652 Bluetooth LE 2M PHY

Families: cyrf, a7105, nrf: the tests of suite_<family>.py (rig.py's
suite_setups()). The packet counter is at byte 2, so --count is at most 256.

Losses locked to the 102.4 ms cycle (--timing: most of them in one phase
window, against 2 / (102.4 ms / period) if random) are Wi-Fi beacons: an
interferer, not the format. The CYRF6936 bind setups (PA 0, about -50 dBm)
lose packets that way; the A7105 500 kbps CRC errors are random in time.
"""

import argparse
import collections
import os
import sys
import time

import rig

BEACON_US = 102400


def family_setups(a) -> tuple[list[str], list[str], int]:
    """(TX setup, RX setup, payload length) for a.family / a.name / a.dir: the
    suite test's, the CC2652 at its trim (a7105: a.trim), channel left out."""
    kw = {"nrf": {"addrs": [a.addr]}, "a7105": {"trim": a.trim}}.get(a.family, {})
    s = rig.suite_setups(a.family, a.name, **kw)
    rev = a.dir == "rev"
    cc = [f"trim {s.case.trim}"] + (s.cc_tx if rev else s.cc_rx)
    return (s.mpm_rx if rev else s.mpm_tx), cc, len(s.payload)


def beacon_phase(lost: list[int], period_us: int) -> tuple[float, float]:
    """Losses in the busiest 2 period wide window of the 102.4 ms cycle, and
    the share expected if the losses were random: (observed, expected)."""
    bins = max(BEACON_US // period_us, 2)
    hist = collections.Counter(s * period_us % BEACON_US * bins // BEACON_US for s in lost)
    best = max(hist[b] + hist[(b + 1) % bins] for b in range(bins))
    return best / len(lost), min(2 / bins, 1.0)


def run_once(tx, rx, payload: bytes, a) -> dict:
    n = len(payload)
    rig.drain(rx)
    rx.cmd(f"rx {n}")
    tx.cmd(f"seq {rig.SEQ}")
    tx.cmd(f"txp {a.period} {a.count} {payload.hex()}")
    tx.wait_event("@ev txdone", a.count * a.period / 1e6 + 2)
    time.sleep(0.2)
    good, bad, other = set(), {}, 0
    rssi = collections.Counter()
    for p in rig.packets(rx, good=False):
        d = p.data
        if len(d) <= rig.SEQ:
            other += 1
            continue
        if (r := p.flags.get("rssi")) is not None:
            rssi[int(r)] += 1
        exp = bytearray(payload)
        exp[rig.SEQ] = d[rig.SEQ]
        body = d[:n]
        if p.crc_ok is False or body != bytes(exp):
            if len(body) < n:
                other += 1
                continue
            bad[d[rig.SEQ]] = [(i, body[i] ^ exp[i]) for i in range(n) if body[i] != exp[i]]
        else:
            good.add(d[rig.SEQ])
    rx_stop, tx_stop = rx.cmd("stop"), tx.cmd("stop")
    bad = {s: v for s, v in bad.items() if s not in good}
    missing = [s for s in range(a.count) if s not in good and s not in bad]
    return {"good": len(good), "bad": bad, "missing": missing, "other": other, "rssi": rssi,
            "rx_stop": rx_stop, "tx_stop": tx_stop}


def report(i: int, r: dict, a):
    lost = sorted(r["missing"] + list(r["bad"]))
    print(f"run {i}: good {r['good']}/{a.count}  bad {len(r['bad'])}  missing {len(r['missing'])}"
          f"  other {r['other']}")
    if a.q:
        return
    if r["rssi"]:
        print(f"   rssi: {dict(sorted(r['rssi'].items()))}")
    print(f"   rx: {r['rx_stop']}\n   tx: {r['tx_stop']}")
    if r["bad"]:
        pos = collections.Counter(b for diff in r["bad"].values() for b, _ in diff)
        bits = collections.Counter(sum(bin(x).count("1") for _, x in diff) for diff in r["bad"].values())
        print(f"   wrong bytes (position: packets): {dict(sorted(pos.items()))}")
        print(f"   wrong bits per packet: {dict(sorted(bits.items()))}")
        for s, diff in list(r["bad"].items())[:a.show]:
            print(f"   seq {s:3d}: " + (" ".join(f"[{b}]^{x:02x}" for b, x in diff) or "payload right"))
    if a.timing and lost:
        print(f"   lost seq: {lost}")
        print(f"   gaps (x {a.period} us): {[y - x for x, y in zip(lost, lost[1:])]}")
        seen, rnd = beacon_phase(lost, a.period)
        print(f"   in one 102.4 ms phase window: {100 * seen:.0f}% (random: {100 * rnd:.0f}%)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("family", choices=("cyrf", "a7105", "nrf"))
    ap.add_argument("name", help="setup name of the family's suite")
    ap.add_argument("--dir", default="fwd", choices=("fwd", "rev"), help="fwd: MPM -> CC2652")
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--count", type=int, default=250, help="packets per run, at most 256")
    ap.add_argument("--period", type=int, help="TX period in us (default: cyrf 10000, a7105 and nrf 5000)")
    ap.add_argument("--ch", type=int, help="channel (default: cyrf 30, a7105 170, nrf 40)")
    ap.add_argument("--trim", type=int, default=90, help="a7105: CC2652 trim in kHz (default 90)")
    ap.add_argument("--addr", default="e7e7e7e7e7", help="nrf: address (default e7e7e7e7e7)")
    ap.add_argument("--payload", help="payload in hex (default: the suites' pattern)")
    ap.add_argument("--mpm-extra", default="", help="commands after the MPM setup")
    ap.add_argument("--cc-extra", default="", help="commands after the CC2652 setup")
    ap.add_argument("--timing", action="store_true", help="lost packets, gaps, 102.4 ms share")
    ap.add_argument("--show", type=int, default=10, help="bad packets printed per run")
    ap.add_argument("-q", action="store_true", help="one line per run")
    a = ap.parse_args()
    if a.count > 256:
        ap.error("--count: at most 256 (8 bit counter)")
    a.period = a.period or (10000 if a.family == "cyrf" else 5000)
    ch = a.ch if a.ch is not None else {"cyrf": 30, "a7105": 170, "nrf": 40}[a.family]
    names = rig.suite_names(a.family)
    if a.name not in names:
        ap.error(f"unknown {a.family} setup {a.name}: {' '.join(names)}")
    rx_2m = any(s in a.cc_extra + os.environ.get("CC_INIT", "") for s in ("sx1281", "nrf2mble"))
    if a.family == "nrf" and a.name == "nrf2m" and a.dir == "fwd" and not rx_2m:
        ap.error('nrf2m: the CC2652 proprietary PHY has no 2 Mbps receiver: --dir rev, '
                 '--cc-extra "preset nrf2mble" (Bluetooth LE 2M PHY), or the SX1281 '
                 '(--cc-extra or CC_INIT "chip sx1281")')

    mpm_setup, cc_setup, n = family_setups(a)
    payload = bytes.fromhex(a.payload) if a.payload else rig.pattern(n)
    if len(payload) != n:
        ap.error(f"--payload: {n} bytes for {a.name}")
    mpm, cc = rig.open_devices()
    with mpm, cc:
        rig.run(mpm, mpm_setup + rig.commands(a.mpm_extra) + [f"ch {ch}"])
        rig.run(cc, cc_setup + rig.commands(a.cc_extra) + [f"ch {ch}"])
        tx, rx = (mpm, cc) if a.dir == "fwd" else (cc, mpm)
        total = collections.Counter()
        for i in range(a.runs):
            r = run_once(tx, rx, payload, a)
            report(i, r, a)
            total.update(good=r["good"], bad=len(r["bad"]), missing=len(r["missing"]), other=r["other"])
        if a.runs > 1:
            print(f"total: good {total['good']}/{a.runs * a.count}  bad {total['bad']}  "
                  f"missing {total['missing']}  other {total['other']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
