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
  proto_compare.py flysky --sub 1
  proto_compare.py v2x2
  proto_compare.py xk --sub 2

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


FLYSKY_TX_CHANNELS = [
    (0x12, 0x34, 0x56, 0x78), (0x18, 0x27, 0x36, 0x45), (0x41, 0x82, 0x36, 0x57), (0x84, 0x13, 0x65, 0x72),
    (0x87, 0x64, 0x15, 0x32), (0x76, 0x84, 0x13, 0x52), (0x71, 0x62, 0x84, 0x35), (0x71, 0x86, 0x43, 0x52)]


def flysky_hop0(addr: bytes, sub: int) -> int:
    """hopping_frequency[0] of FLYSKY_init()"""
    a3 = addr[3] - 0x70 if (addr[3] & 0xF0) > 0x90 else addr[3]
    row, offset = a3 & 0x0F, a3 // 16
    hop = [0] * 16
    for i in range(16):
        t = FLYSKY_TX_CHANNELS[row >> 1][i >> 2]
        t = (t & 0x0F if i & 2 else t >> 4) * 0x0A + (0x50 if i & 1 else 0)
        if sub == 4:  # CX20
            if t == 0x0A:
                t += 0x37
            if t == 0xA0:
                t = 0x37 if offset < 4 else 0x2D if offset < 9 else 0x29
        hop[15 - i if row & 1 else i] = t - offset
    return hop[0]


def flysky_capture(addr: bytes, sub: int) -> list[Capture]:
    """Bind on A7105 channel 1, data on hopping_frequency[0]"""
    return [Capture("bind", "preset flysky;ch 1", 21, bind=True),
            Capture("data", f"preset flysky;ch {flysky_hop0(addr, sub)}", 21)]


V2X2_FREQ = [  # freq_hopping[][] of V2X2_nrf24l01.ino
    [0x27, 0x1B, 0x39, 0x28, 0x24, 0x22, 0x2E, 0x36, 0x19, 0x21, 0x29, 0x14, 0x1E, 0x12, 0x2D, 0x18],
    [0x2E, 0x33, 0x25, 0x38, 0x19, 0x12, 0x18, 0x16, 0x2A, 0x1C, 0x1F, 0x37, 0x2F, 0x23, 0x34, 0x10],
    [0x11, 0x1A, 0x35, 0x24, 0x28, 0x18, 0x25, 0x2A, 0x32, 0x2C, 0x14, 0x27, 0x36, 0x34, 0x1C, 0x17],
    [0x22, 0x27, 0x17, 0x39, 0x34, 0x28, 0x2B, 0x1D, 0x18, 0x2A, 0x21, 0x38, 0x10, 0x26, 0x20, 0x1F]]


def v2x2_hop0(addr: bytes) -> int:
    """hopping_frequency[0] of V2X2_set_tx_id(): row = sum of addr[1..3] & 3,
    increment its bits 2 to 4, channels divisible by 16 lowered by 3"""
    s = sum(addr[1:4]) & 0xFF
    v = V2X2_FREQ[s & 3][0] + ((s & 0x1E) >> 2)
    return v if v & 0x0F else v - 3


def v2x2_capture(addr: bytes, sub: int) -> list[Capture]:
    """Bind (flags c0, 4 s) and data on hopping_frequency[0], address 66 88 68 68 68"""
    nrf = f"preset nrf1m;nrf 6688686868 2;ch {v2x2_hop0(addr)}"
    return [Capture("bind", nrf, 16, bind=True), Capture("data", nrf, 16)]


XK_TX_ADDR = bytes([  # XK_tx_addr[] of XK_ccnrf.ino
    0xB3, 0x67, 0xE9, 0x98, 0x3A, 0xEC, 0xA6, 0x59, 0xB2, 0x94, 0x2B, 0xA5, 0x37, 0xC5, 0x4A, 0xD3,
    0x49, 0xA6, 0x83, 0xEB, 0x4B, 0xC9, 0x59, 0xD2, 0x65, 0x34, 0x6A, 0xD3, 0x2C, 0x96, 0x2A, 0xA9,
    0x32, 0xB2, 0xB4, 0x49, 0xD3, 0x37, 0xE9])
XK_HOP = bytes([  # XK_hop[]
    0x47, 0x3A, 0x4C, 0x39, 0x4D, 0x34, 0x4A, 0x3F, 0x45, 0x3E, 0x4B, 0x3D, 0x3B, 0x48, 0x40, 0x49,
    0x46, 0x3C, 0x43, 0x38, 0x35, 0x42, 0x33, 0x44, 0x4E, 0x37, 0x44, 0x35, 0x37, 0x4E, 0x36, 0x41])


def xk_ok(p: bytes) -> bool:
    return len(p) == 16 and sum(p[:15]) & 0xFF == p[15]


def xk_capture(addr: bytes, sub: int) -> list[Capture]:
    """Bind (address 68 94 a6 d5 c3, XK_bind_hop[0], flag c0) and data on
    hopping_frequency[8], both from XK_initialize_txid(): sum of addr[1..3]
    picks the 4 data channels and the address. X450 is 250 kbps"""
    s = sum(addr[1:4]) & 0xFF
    start = (s & 0x1F) + ((s >> 5) & 3)
    xn = f"xn 1 1 {int(sub == 0)}"
    return [Capture("bind", f"{xn};xaddr 6894a6d5c3;ch 7", 16, bind=True, valid=xk_ok),
            Capture("data", f"{xn};xaddr {XK_TX_ADDR[start:start + 5].hex()};ch {XK_HOP[(s & 7) << 2]}", 16,
                    valid=xk_ok)]


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
CASES = {"flysky": (1, flysky_capture), "bayang": (14, bayang_capture), "frskyd": (3, frskyd_capture),
         "frskyv": (25, frskyv_capture), "frskyx": (15, frskyx_capture),
         "frskyx2": (64, frskyx2_capture),
         "afhds2a": (28, afhds2a_capture), "dsm": (6, dsm_capture), "v2x2": (5, v2x2_capture),
         "xk": (62, xk_capture)}


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
