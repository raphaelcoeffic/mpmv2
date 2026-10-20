#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""A protocol ported to the MPM core (src/mpm/upstream) against the MULTI-Module
running the same protocol: same module id, rx_num and channels. The MPM v2
(bench firmware) receives each on one channel and address, in the bind phase
and after; the payloads and packet intervals are compared.

  proto_compare.py bayang
  proto_compare.py bayang --secs 6 --sub 1
  proto_compare.py dsm --sub 3

Devices: the LaunchPad runs the firmware without BENCH (MPM core, driven by
radio_sim.py's frames over its XDS110 port), the 4-in-1 the MPM bench
firmware (its "id" command), the MPM v2 the mpm-v2 bench firmware.
"""

from __future__ import annotations

import argparse
import statistics
import sys
import time
from collections import Counter
from typing import NamedTuple

import rig
from mpm_bench import Bench, find_port
from radio_sim import Radio

# Channel values sent to both: sticks off center, CH5 and CH7 on
CHANNELS = [1300, 700, 400, 1600, 1800, 204, 1800, 204] + [1024] * 8


def rx_tx_addr(module_id: int, rx_num: int) -> bytes:
    i = (module_id + rx_num) & 0xFFFFFFFF
    a = i.to_bytes(4, "big")
    return a + bytes([(a[2] & 0xF0) | (a[3] & 0x0F)])


class Capture(NamedTuple):
    name: str
    setup: str           # receiver (MPM v2 bench) setup
    rx_len: int
    bind: bool = False   # started with the bind flag
    mask: tuple = ()     # payload bytes left out of the comparison (counters, CRCs)
    valid: object = None  # payload check (protocol CRC without the chip's)


def bayang_capture(addr: bytes, sub: int) -> list[Capture]:
    """Bind channel/address, then a data channel."""
    bind_ch = 10 if sub in (2, 3) else 0  # X16_AH, IRDRONE
    data_ch = (addr[3] & 0x1F) + 0x10  # hopping_frequency[1]
    xn = "preset nrf1m;xn 1 1 0"
    return [Capture("bind", f"{xn};xaddr 0000000000;ch {bind_ch}", 15),
            Capture("data", f"{xn};xaddr {addr.hex()};ch {data_ch}", 15)]


def frskyd_capture(addr: bytes, sub: int) -> list[Capture]:
    """Bind and data on CHANNR 0 (hopping_frequency[0] = 0 for every ID).
    Data: frame and telemetry counters left out."""
    return [Capture("bind", "preset frskyd;ch 0", 20, bind=True),
            Capture("data", "preset frskyd;ch 0", 20, mask=(3, 4))]


def frskyv_capture(addr: bytes, sub: int) -> list[Capture]:
    """Bind on CHANNR 0, data on CHANNR 6 (channel 0 of the seed sequence):
    seed and CRC left out."""
    return [Capture("bind", "preset frskyv;ch 0", 16, bind=True),
            Capture("data", "preset frskyv;ch 6", 16, mask=(3, 4, 14))]


def frskyx_crc_ok(p: bytes) -> bool:
    """FrSkyX_crc() over packet[3..len-2], in the last 2 bytes"""
    crc = 0
    for b in p[3:-2]:
        v = (crc >> 8) ^ b
        crc = ((crc << 8) ^ FRSKYX_CRC_SHORT[v & 15] ^ (0x1081 * (v >> 4))) & 0xFFFF
    return len(p) > 5 and p[-2:] == crc.to_bytes(2, "big")


FRSKYX_CRC_SHORT = [0x0000, 0x1189, 0x2312, 0x329B, 0x4624, 0x57AD, 0x6536, 0x74BF,
                    0x8C48, 0x9DC1, 0xAF5A, 0xBED3, 0xCA6C, 0xDBE5, 0xE97E, 0xF8F7]


def frskyx_capture(addr: bytes, sub: int, x2: bool = False) -> list[Capture]:
    """Bind on CHANNR 0, data on hopping_frequency[0] (rx_tx_addr[0] & 7):
    channel skip (random), hop index, failsafe flag, sequence and CRC left
    out. Sub 2, 3: LBT (EU). X2: the chip's CRC too"""
    lbt = sub in (2, 3)
    n = 0x20 if lbt and not x2 else 0x1D
    preset = "frskyx" + ("2" if x2 else "") + ("eu" if lbt else "")
    # hopping_frequency[0]: v1 FCC rx_tx_addr[0] & 7, X2 (FrSkyX2_init_hop)
    # the offset of its ID; FCC offset 3, 4 -> 5, 6, LBT 0, 1 -> 2, 3
    ch = addr[0] & 7
    offset = ((addr[2] << 8) + addr[3]) % 5 if x2 else (ch - 2 if ch >= 5 else ch)
    if x2 or lbt:
        ch = offset + 2 if (offset <= 1 if lbt else offset >= 3) else offset
    return [Capture("bind", f"preset {preset};ch 0", n + 1, bind=True, valid=frskyx_crc_ok),
            Capture("data", f"preset {preset};ch {ch}", n + 1,
                    mask=(4, 5, 7, 21, 22, n - 1, n), valid=frskyx_crc_ok)]


def frskyx2_capture(addr: bytes, sub: int) -> list[Capture]:
    return frskyx_capture(addr, sub, x2=True)


def afhds2a_hop0(module_id: int, addr: bytes) -> int:
    """hopping_frequency[0] of AFHDS2A_calc_channels()"""
    rnd = module_id
    band_no = addr[3] & 3  # idx 0
    rnd = (rnd * 0x0019660D + 0x3C6EF35F) & 0xFFFFFFFF
    return band_no * 41 + 1 + (rnd % 41)


def afhds2a_capture(addr: bytes, sub: int, module_id: int = 0) -> list[Capture]:
    """Bind on A7105 channel 0x0D, data on hopping_frequency[0]: the RX ID
    (EEPROM: from earlier binds on the MPM) left out"""
    rx_id = (5, 6, 7, 8)
    return [Capture("bind", "preset afhds2a;ch 13", 38, bind=True, mask=rx_id),
            Capture("data", f"preset afhds2a;ch {afhds2a_hop0(module_id, addr)}", 38, mask=rx_id)]


def dsm_pncodes() -> list[bytes]:
    """DSM_pncodes of DSM.ino (Row 1 to 4 then Row 0, 8 or 9 columns)"""
    import re
    from pathlib import Path
    src = (Path(__file__).parents[2] / "src/mpm/upstream/DSM.c").read_text()
    table = src[src.index("DSM_pncodes"):]
    table = re.sub(r"//[^\n]*|/\*.*?\*/", "", table[:table.index("};")], flags=re.S)
    return [bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", row))
            for row in re.findall(r"\{([^{}]*)\}", table)]


def dsm_read_code(codes: list[bytes], row: int, col: int) -> bytes:
    """DSM_read_code()"""
    row = (row - 1) & 0xFF
    return codes[min(row, 4) * 8 + col]


def dsm_cyrf_id(module_id: int) -> bytes:
    """The port's CYRF6936 MFG ID (cyrf6936.c): the module id, 2 zero bytes"""
    return module_id.to_bytes(4, "big") + bytes(2)


def dsmx_channels(mfg: list[int]) -> list[int]:
    """DSM_calc_dsmx_channel()"""
    chans = []
    id_tmp = ~((mfg[0] << 24) | (mfg[1] << 16) | (mfg[2] << 8) | mfg[3]) & 0xFFFFFFFF
    while len(chans) < 23:
        id_tmp = (id_tmp * 0x0019660D + 0x3C6EF35F) & 0xFFFFFFFF
        ch = ((id_tmp >> 8) % 0x49) + 3
        if (ch ^ mfg[3]) & 1 or ch in chans:
            continue
        counts = [sum(1 for c in chans if lo <= c <= hi) for lo, hi in ((0, 27), (28, 51), (52, 255))]
        if (ch < 28 and counts[0] < 8) or (28 <= ch < 52 and counts[1] < 7) or (ch >= 52 and counts[2] < 8):
            chans.append(ch)
    return chans


def dsm_bind_ok(p: bytes) -> bool:
    """DSM_build_bind_packet() sums (SDR bind packets have no CRC: corrupted
    ones are received, e.g. by WiFi beacons over their 8 ms)"""
    s1 = 384 - 0x10 + sum(p[:8])
    s2 = s1 + sum(p[8:14])
    return len(p) == 16 and p[8:10] == (s1 & 0xFFFF).to_bytes(2, "big") and p[14:16] == (s2 & 0xFFFF).to_bytes(2, "big")


def dsm_capture(addr: bytes, sub: int, module_id: int = 0, rx_num: int = 0) -> list[Capture]:
    """DSMX: bind on channel 13 (SDR), data on hopping_frequency[0] with its
    SOP / data codes and the CH2 CRC seed (the first packet there; it
    alternates with the CH1 seed, those fail the CRC)"""
    if sub not in (2, 3):
        sys.exit("dsm: DSMX only, --sub 2 (X 1F) or 3 (X 2F)")
    mfg = list(dsm_cyrf_id(module_id)[:4])
    mfg[3] ^= rx_num
    sop_col = (mfg[0] + mfg[1] + mfg[2] + 2) & 7
    if sop_col == 0:
        mfg[addr[0] % 3] ^= 1
        sop_col = (mfg[0] + mfg[1] + mfg[2] + 2) & 7
    seed = (mfg[0] << 8) + mfg[1]
    ch = dsmx_channels(mfg)[0]
    codes = dsm_pncodes()
    row = (ch - 2) % 5
    sop = dsm_read_code(codes, row, sop_col)
    data = dsm_read_code(codes, row, 7 - sop_col) + dsm_read_code(codes, row, 8 - sop_col)
    return [Capture("bind", "preset dsmbind;ch 13", 16, bind=True, valid=dsm_bind_ok),
            Capture("data", f"preset dsm;cyrf {sop.hex()} {data.hex()} {seed};ch {ch}", 16)]


# name: (protocol number, captures(addr, sub))
CASES = {"bayang": (14, bayang_capture), "frskyd": (3, frskyd_capture),
         "frskyv": (25, frskyv_capture), "frskyx": (15, frskyx_capture),
         "frskyx2": (64, frskyx2_capture),
         "afhds2a": (28, afhds2a_capture), "dsm": (6, dsm_capture)}


def masked(p: str, mask: tuple) -> str:
    b = bytearray.fromhex(p)
    for i in mask:
        if i < len(b):
            b[i] = 0
    return b.hex()


def capture(rx: Bench, setup: str, rx_len: int, start, stop, secs: float) -> list[tuple[int, str]]:
    rig.run(rx, setup)
    rig.drain(rx)
    rx.cmd(f"rx {rx_len}")
    start()
    time.sleep(secs)
    stop()
    rx.cmd("stop")
    time.sleep(0.1)
    out = []
    for e in rig.events(rx):
        f = e.split()
        if len(f) >= 4 and "crc=bad" not in e:
            out.append((int(f[1]), f[3]))
    return out


def summary(pkts: list[tuple[int, str]]) -> str:
    if not pkts:
        return "no packets"
    gaps = [b[0] - a[0] for a, b in zip(pkts, pkts[1:])]
    med = statistics.median(gaps) if gaps else 0
    return f"{len(pkts)} packets, {len(set(p for _, p in pkts))} distinct, median interval {med} us"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("case", choices=CASES)
    ap.add_argument("--sub", type=int, default=0)
    ap.add_argument("--rx-num", type=int, default=0)
    ap.add_argument("--option", type=int, default=0)
    ap.add_argument("--id", type=lambda s: int(s, 0), default=0x1CA156B8,
                    help="the LaunchPad's module id (IEEE address, last 32 bits)")
    ap.add_argument("--secs", type=float, default=4.0)
    a = ap.parse_args()

    proto, captures = CASES[a.case]
    addr = rx_tx_addr(a.id, a.rx_num)
    fails = 0
    with Radio(find_port("cc")) as radio, Bench(find_port("mpm")) as mpm, \
            Bench(find_port("v2"), 921600) as rx:
        rig.run(rx, "chip cc2652")
        mpm.cmd(f"id {a.id:#x}")
        for i, v in enumerate(CHANNELS):
            mpm.cmd(f"chan {i} {v}")
        radio.set(protocol=0, channels=CHANNELS)  # protocol list: no RF until started

        kw = {"module_id": (a.id + a.rx_num) & 0xFFFFFFFF} if a.case == "afhds2a" else {}
        if a.case == "dsm":
            # The MPM's DSM ID: the port's CYRF6936 MFG ID instead of its chip's
            mpm.cmd(f"cyrfid {dsm_cyrf_id(a.id).hex()}")
            kw = {"module_id": a.id, "rx_num": a.rx_num}
        for c in captures(addr, a.sub, **kw):
            def mpm_start():
                mpm.cmd(f"proto {proto} {a.sub} {a.rx_num} {a.option} {int(c.bind)}")

            def port_start():
                radio.set(protocol=proto, sub=a.sub, rx_num=a.rx_num, option=a.option, bind=c.bind)

            got = {}
            for src, start, stop in (("MPM", mpm_start, lambda: mpm.cmd("stop")),
                                     ("port", port_start, lambda: radio.set(protocol=0, bind=False))):
                time.sleep(0.2)
                got[src] = capture(rx, c.setup, c.rx_len, start, stop, a.secs)
                if c.valid:
                    got[src] = [(t, p) for t, p in got[src] if c.valid(bytes.fromhex(p))]
                print(f"{c.name:5s} {src:4s}: {summary(got[src])}")
            ref = Counter(masked(p, c.mask) for _, p in got["MPM"])
            port = Counter(masked(p, c.mask) for _, p in got["port"])
            for p, n in ref.most_common(3):
                print(f"      {p}  MPM {n}, port {port.get(p, 0)}")
            only = [p for p in port if p not in ref]
            for p in only[:3]:
                print(f"      {p}  port only ({port[p]})")
            ok = bool(ref) and set(port) == set(ref)
            print(f"      {'same payloads' if ok else 'DIFFERENT'}")
            fails += not ok
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
