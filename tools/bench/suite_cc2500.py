#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""CC2500 family: the MPM CC2500 with each protocol's registers against the
CC2652 preset of the same name, both ways (link test, all bytes compared).

  suite_cc2500.py                  all
  suite_cc2500.py hott hitec       some
  suite_cc2500.py --phy hitec      print the CC2652 PHY derived from the MPM registers

The payload length and period come from the MPM registers once set up
(PKTLEN, PKTCTRL0, data rate, preamble, FEC). The MPM receives one packet per
FIFO read (RXOFF_MODE IDLE, "rx 0") and checks CRC_OK. --trim: frequency
correction of the CC2652 for the CC2500 (MPM option), -35 kHz on our pair.
"""

import argparse
import sys

import rig

# name: (MPM setup, CC2652 setup, payload length or 0 = from the registers).
# Channel 0 unless the CC2652 setup has a "ch" (the MPM setups set it)
P = {
    "frskyd": ("frskyd.txt", "preset frskyd", 18),
    "frskyx": ("preset frskyx;ch 0", "preset frskyx", 30),
    "frskyx2": ("preset frskyx;w 0x11 0x84;w 0x08 0x05;ch 0", "preset frskyx2", 30),
    "frskyxeu": ("preset frskyxeu;ch 0", "preset frskyxeu", 30),
    "frskyx2eu": ("preset frskyxeu;w 0x08 0x05;ch 0", "preset frskyx2eu", 30),
    "frskyv": ("frskyv.txt", "preset frskyv", 0),
    "hott": ("hott.txt", "preset hott", 0),
    "hitec": ("hitec.txt", "preset hitec", 0),
    "redpine": ("redpine.txt", "preset redpine", 0),
    "redpineslow": ("redpineslow.txt", "preset redpineslow", 0),
    "skyartec": ("skyartec.txt", "preset skyartec;cc2500 7d7d7d7d 1 1 0 0 1", 0),
    "ikea": ("ikea.txt", "preset ikea;ch 16", 0),
    "corona": ("corona.txt", "preset corona", 0),
    # V2 freezes the CC2500 AGC: lower the CC2652 power
    "coronav2": ("corona.txt;w 0x0e 8000;w 0x15 0x50;w 0x1b 67fbdc;ch 0", "pwr 0x10c3;preset coronav2", 0),
    "fdv3": ("corona.txt;w 0x15 0x50;ch 0", "preset fdv3", 0),
    "ares": ("ares.txt", "preset ares", 0),
    "radiolink": ("radiolink.txt", "preset radiolink", 0),
    "sfhss": ("sfhss.txt", "preset sfhss", 0),
}

X = 26e6  # CC2500 crystal
BW = {85: 174.8, 86: 220.4, 87: 264.9, 88: 307.3, 89: 349.7, 90: 440.8, 91: 529.8, 92: 614.6}


def phy(r: bytes) -> dict:
    """CC2500 registers (from 0x00) -> packet format and an equivalent CC2652 PHY / framing."""
    rate = (256 + r[0x11]) * 2 ** (r[0x10] & 15) / 2 ** 28 * X
    mod = (r[0x12] >> 4) & 7
    dm, de = r[0x15] & 7, (r[0x15] >> 4) & 7
    # MSK: 2-FSK, rate / 4 for a full phase step, 0.32 x rate for a partial one
    dev = (rate / 4 if dm == 7 else 0.32 * rate) if mod == 7 else X / 2 ** 17 * (8 + dm) * 2 ** de
    sync = r[4:6].hex() * (2 if r[0x12] & 3 == 3 else 1)
    pre = [2, 3, 4, 6, 8, 12, 16, 24][(r[0x13] >> 4) & 7]
    fec = r[0x13] >> 7
    crc, white, var, cc2400 = (r[0x08] >> 2) & 1, (r[0x08] >> 6) & 1, (r[0x08] & 3) == 1, (r[0x08] >> 3) & 1
    base = int(round((r[0x0D] << 16 | r[0x0E] << 8 | r[0x0F]) * X / 65536))
    spacing = int(round(X / 2 ** 18 * (256 + r[0x14]) * 2 ** (r[0x13] & 3)))
    need = (0.32 * rate * 2 + rate) if mod == 7 else (2 * dev + rate + 70e3)
    bw = 89 if mod == 7 else min(c for c, k in BW.items() if k * 1e3 >= need)
    dev = int(round(dev / 250) * 250)
    return {"rate": rate, "pre": pre, "fec": fec, "var": var, "pktlen": r[0x06], "cmds": [
        f"phy {int(round(rate))} {dev} {bw} {pre} 0x8000 {0 if mod == 1 else 1}",
        f"cc2500 {sync} {int(crc and not cc2400) + 2 * (crc and cc2400)} {int(var)} {white} {fec} {int(mod == 7)}",
        f"chplan {base} {spacing}"]}


def cases(mpm, names=None, trim: int = -35, period: int = 0, count: int = 200, direction: str = "both"):
    """rig.Case of each test; sets the MPM up for each (its registers give the
    packet format)."""
    for name in names or list(P):
        mpm_setup, cc_setup, n = P[name]
        rig.run(mpm, mpm_setup)
        p = phy(bytes.fromhex(mpm.cmd("r 0x00 0x2f")))
        n = n or (min(p["pktlen"], 40) if p["var"] else p["pktlen"])
        payload = rig.pattern(n)
        if p["var"]:
            payload = bytes([n - 1]) + payload[1:]
        airtime = (n + p["pre"] + 6) * 8 * (2 if p["fec"] else 1) / p["rate"] * 1e6
        if "ch " not in cc_setup:
            cc_setup += ";ch 0"
        pwr = None
        if cc_setup.startswith("pwr "):
            pwr, cc_setup = cc_setup[4:].split(";", 1)
        yield rig.Case("cc2500", name, payload, mpm_setup, cc_setup, trim=trim, pwr=pwr,
                       kw=dict(period=period or max(3000, int(airtime) + 2000), count=count,
                               direction=direction, cc2500=True))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", help=f"protocols (default: all): {' '.join(P)}")
    ap.add_argument("--trim", type=int, default=-35, help="CC2652 trim in kHz (default -35)")
    ap.add_argument("--phy", action="store_true", help="only print the PHY derived from the MPM registers")
    rig.common_args(ap, period=0, count=200)
    a = ap.parse_args()
    fails = 0
    mpm, cc = rig.open_devices()
    with mpm, cc:
        if a.phy:
            for name in a.names or list(P):
                rig.run(mpm, P[name][0])
                p = phy(bytes.fromhex(mpm.cmd("r 0x00 0x2f")))
                print(f"{name}: rate={p['rate']:.0f} preamble={p['pre']} PKTLEN={p['pktlen']}\n  "
                      + "\n  ".join(p["cmds"]))
            return 0
        for c in cases(mpm, a.names, a.trim, a.period, a.count, a.dir):
            # pwr applies at once and is kept: the default unless the test sets one
            r = rig.run_case(mpm, cc, c, pwr="default", verbose=a.verbose)
            rig.print_results(c.name, r)
            fails += sum(v["good"] < v["count"] for v in r.values())
        cc.cmd("pwr default")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
