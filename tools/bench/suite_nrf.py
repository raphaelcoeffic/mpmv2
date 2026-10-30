#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""NRF24L01 and XN297 emulation: MPM <-> CC2652, both ways, 16 byte payload, ch 40.

  suite_nrf.py                         all
  suite_nrf.py nrf1m --addr 6565656565
  suite_nrf.py xn297 xn297enh

nrf1m / nrf250k / nrf2m / nrf2mble: plain NRF24L01 (static length, CRC16) for
each --addr (default: the addresses of docs/interop-results.md). 2 Mbps:
nrf2mble on the CC2652 Bluetooth LE 2M PHY, both ways; nrf2m on its
proprietary PHY, CC2652 -> MPM only (no 2 Mbps receiver, 2-3% lost at ch 40). The XN297 setups are the same commands on
both sides. The unscrambled enhanced mode needs the MPM XN297 emulation fix
(branch xn297-enhanced-xorout of the MPM fork) for MPM -> CC2652.
"""

import argparse
import sys

import rig

ADDRS = ["e7e7e7e7e7", "c3c3c3c3c3", "6565656565", "a1b2c3d4e5", "0102030405", "0f0f0f0f0f"]
NRF = {"nrf1m": ("", "nrf1m"), "nrf250k": ("w 0x06 0x22", "nrf250k"), "nrf2m": ("w 0x06 0x0a", "nrf2m"),
       "nrf2mble": ("w 0x06 0x0a", "nrf2mble")}
XN = "xaddr 6545a5e7c3;ch 40"
# name: (MPM setup, CC2652 setup or None = the same)
P = {
    "xn297": ("xn297_1m.txt", None),
    "xn297_250k": ("xn297_250k_cc2500.txt", None),
    "xn297_250k_nrf": (f"xn 1 1 1 1;{XN}", "xn297_250k_cc2500.txt"),
    "xn297_unscr3": ("xn 1 0 0;xaddr 6545a5;ch 40", None),
    "xn297_nocrc4": ("xn 0 1 0;xaddr 6545a5e7;ch 40", None),
    "xn297enh": (f"xn 1 1 0 0 1;{XN}", None),
    "xn297enh_250k": (f"xn 1 1 1 0 1;{XN}", None),
    "xn297enh_unscr": (f"xn 1 0 0 0 1;{XN}", None),
    "xn297enh_unscr_250k": (f"xn 1 0 1 0 1;{XN}", None),
}


def nrf_setups(name: str, addr: str) -> tuple[str, str]:
    """(MPM setup, CC2652 setup) of a plain NRF24L01 test, without the channel."""
    rf, preset = NRF[name]
    return f"nrf_1m.txt;{rf};w 0x0a {addr};w 0x10 {addr}", f"preset {preset};nrf {addr} 2"


def cases(names=None, addrs=None, period: int = 5000, count: int = 250, direction: str = "both"):
    """rig.Case of each test (plain NRF24L01: one per address)."""
    payload = rig.pattern(16)
    for name in names or list(NRF) + list(P):
        if name in NRF:
            for addr in addrs or ADDRS:
                mpm_setup, cc_setup = nrf_setups(name, addr)
                yield rig.Case("nrf", f"{name} {addr}", payload, mpm_setup, cc_setup,
                               kw=dict(ch=40, period=period, count=count,
                                       direction="rev" if name == "nrf2m" else direction))
            continue
        mpm_setup, cc_setup = P[name]
        yield rig.Case("nrf", name, payload, mpm_setup, cc_setup or mpm_setup,
                       kw=dict(period=period, count=count, direction=direction))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", help=f"tests (default: all): {' '.join(list(NRF) + list(P))}")
    ap.add_argument("--addr", action="append", help="NRF addresses (TX_ADDR order), repeatable")
    rig.common_args(ap, period=5000, count=250)
    a = ap.parse_args()
    fails = 0
    mpm, cc = rig.open_devices()
    with mpm, cc:
        for c in cases(a.names, a.addr, a.period, a.count, a.dir):
            r = rig.run_case(mpm, cc, c, verbose=a.verbose)
            rig.print_results(c.name, r)
            fails += sum(v["good"] < v["count"] for v in r.values())
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
