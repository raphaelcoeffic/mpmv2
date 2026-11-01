"""Shared helpers of the interoperability tests: the two bench devices (a 4-in-1
MULTI-Module with the bench firmware and the mpm-v2 CC2652), their setups, the
received packets, the two-way link test and the suites' tests. See docs/bench.md.

Ports: $MPM_PORT / $CC_PORT / $V2_PORT, else detected (mpm_bench.find_port()).
A setup is a list of commands separated by ';'. An item ending in .txt is a
command file, looked up in examples/ when it isn't a path:
  "preset frskyd;ch 0", "hott.txt", "cc2652_dsm.txt;prefix a5e01b56"
"""

from __future__ import annotations

import dataclasses
import importlib
import os
import time
from dataclasses import dataclass, field
from pathlib import Path

from mpm_bench import Bench, RxPacket, find_port, setup_lines

EXAMPLES = Path(__file__).resolve().parent / "examples"
SEQ = 2  # payload byte replaced by the packet counter


def commands(spec: str | list[str] | None) -> list[str]:
    if not spec:
        return []
    items = spec if isinstance(spec, list) else spec.split(";")
    out = []
    for item in (i.strip() for i in items):
        if item.endswith(".txt"):
            path = Path(item) if Path(item).exists() else EXAMPLES / item
            out += [line.split("#", 1)[0].strip() for line in setup_lines(str(path))]
        elif item:
            out.append(item)
    return [c for c in out if c]


def run(dev: Bench, spec, verbose: bool = False):
    for c in commands(spec):
        r = dev.cmd(c)
        if verbose:
            print(f"  {dev.name}> {c}" + (f" -> {r}" if r else ""), flush=True)


def open_devices() -> tuple[Bench, Bench]:
    """(MPM, CC2652); the CC2652 bench runs on a 921600 baud UART.
    CC_PORT=v2 uses the MPM v2 instead of the LaunchPad. The CC2652 bench is
    set back to its own radio ("chip cc2652"), then $CC_INIT runs on it, e.g.
    CC_INIT="pwr 0x04c6" (applies at the next preset), CC_INIT="chip sx1281"
    (MPM v2: the SX1281 sends and receives, GFSK 125 kbps to 2 Mbps)."""
    mpm, cc = Bench(find_port("mpm")), Bench(find_port("cc"), 921600)
    run(cc, "chip cc2652")
    run(cc, os.environ.get("CC_INIT"))
    return mpm, cc


def drain(dev: Bench):
    while not dev.events.empty():
        dev.events.get_nowait()


def events(dev: Bench, prefix: str = "@rx") -> list[str]:
    out = []
    while not dev.events.empty():
        e = dev.events.get_nowait()
        if e.startswith(prefix):
            out.append(e)
    return out


def packets(dev: Bench, good: bool = True) -> list[RxPacket]:
    """The packets received since the last call (events()); good: without
    the ones with crc=bad."""
    pkts = [RxPacket.parse(e) for e in events(dev)]
    return [p for p in pkts if p.crc_ok is not False] if good else pkts


def pattern(n: int) -> bytes:
    """Test payload: no long runs, every byte different from its neighbours."""
    return bytes((0xA5 + 0x3B * i) & 0xFF for i in range(n))


def flag(line: str, name: str) -> str | None:
    for p in line.split()[4:]:
        if p.startswith(name + "="):
            return p[len(name) + 1:]
    return None


def send_and_count(tx: Bench, rx: Bench, payload: bytes, rx_len: int, period: int, count: int,
                   seq: int = SEQ, status_crc: bool = False) -> dict:
    """tx sends count numbered packets, rx counts the ones with all bytes right.

    status_crc: rx is a CC2500 reading its FIFO with the appended status, check CRC_OK.
    """
    drain(rx)
    rx.cmd(f"rx {rx_len}")
    tx.cmd(f"seq {seq}")
    tx.cmd(f"txp {period} {count} {payload.hex()}")
    tx.wait_event("@ev txdone", count * period / 1e6 + 2)
    time.sleep(0.2)
    seen, good, crc_bad, other = set(), 0, 0, 0
    for p in packets(rx, good=False):
        d = p.data
        body = d[:len(payload)]
        exp = bytearray(payload)
        if len(body) > seq:
            exp[seq] = body[seq]
        if p.crc_ok is False:
            crc_bad += 1
            continue
        if body != bytes(exp):
            other += 1
            continue
        if status_crc and len(d) >= len(payload) + 2 and not d[len(payload) + 1] & 0x80:
            crc_bad += 1
            continue
        seen.add(body[seq])
        good += 1
    rx.cmd("stop")
    tx.cmd("stop")
    return {"distinct": len(seen), "expected": min(count, 256), "good": good, "count": count,
            "crc_bad": crc_bad, "other": other}


def fmt(r: dict) -> str:
    return (f"{100 * r['good'] / r['count']:5.1f}%  ({r['good']}/{r['count']}, distinct "
            f"{r['distinct']}/{r['expected']}, crc_bad={r['crc_bad']}, other={r['other']})")


def link_test(mpm: Bench, cc: Bench, payload: bytes, *, mpm_setup, cc_setup, mpm_rx_setup=None,
              cc_rx_setup=None, ch: int | None = None, mpm_ch: int | None = None, period: int = 10000,
              count: int = 200, direction: str = "both", cc2500: bool = False, seq: int = SEQ,
              verbose: bool = False) -> dict:
    """MPM -> CC2652 ("fwd") then CC2652 -> MPM ("rev"), each after its own setup.

    mpm_rx_setup / cc_rx_setup: added to the setups for the "rev" direction (the
    MPM receiving with a TX-only register setup needs help). ch: appended to both
    setups, mpm_ch: MPM channel if different. cc2500: the MPM CC2500 receives one
    packet per FIFO read (RXOFF_MODE IDLE, "rx 0"), CRC_OK checked when CRC_EN.
    """
    results = {}
    mch = mpm_ch if mpm_ch is not None else ch

    def setup(extra_mpm=None, extra_cc=None):
        run(mpm, commands(mpm_setup) + commands(extra_mpm) + ([f"ch {mch}"] if mch is not None else []), verbose)
        run(cc, commands(cc_setup) + commands(extra_cc) + ([f"ch {ch}"] if ch is not None else []), verbose)

    if direction in ("both", "fwd"):
        setup()
        results["fwd"] = send_and_count(mpm, cc, payload, len(payload), period, count, seq)
    if direction in ("both", "rev"):
        setup(mpm_rx_setup, cc_rx_setup)
        hw_crc = False
        if cc2500:
            mpm.cmd("w 0x17 0x00")
            hw_crc = bool(int(mpm.cmd("r 0x08").split()[-1], 16) & 0x04)
        results["rev"] = send_and_count(cc, mpm, payload, 0 if cc2500 else len(payload), period, count,
                                        seq, status_crc=hw_crc)
    return results


@dataclass
class Case:
    """One test of a suite: link_test() arguments without the CC2652's trim and
    power, so that a run can choose them (the suite's own main, compare.py).

    trim: the suite's trim for the CC2652 LaunchPad. pwr: a TX power word the
    test needs ("0x10c3"), None: the device's default. family: the suite.
    """
    family: str
    name: str
    payload: bytes
    mpm_setup: str
    cc_setup: str
    trim: int = 0
    pwr: str | None = None
    kw: dict = field(default_factory=dict)  # other link_test() arguments


def run_case(mpm: Bench, cc: Bench, case: Case, *, trim: int | None = None, pwr: str | None = None,
             count: int | None = None, direction: str | None = None, verbose: bool = False) -> dict:
    """link_test() of a case. trim: instead of the case's; pwr: the power word
    for the cases without one (None: the power stays as it is); count,
    direction: override (a direction the case doesn't run: no result)."""
    kw = dict(case.kw)
    if count is not None:
        kw["count"] = count
    if direction is not None:
        d = kw.get("direction", "both")
        if d not in ("both", direction):
            return {}
        kw["direction"] = direction
    p = case.pwr or pwr
    cc_setup = (f"pwr {p};" if p else "") + f"trim {case.trim if trim is None else trim};{case.cc_setup}"
    return link_test(mpm, cc, case.payload, mpm_setup=case.mpm_setup, cc_setup=cc_setup, verbose=verbose, **kw)


def print_results(name: str, results: dict):
    for way, label in (("fwd", "MPM -> CC2652"), ("rev", "CC2652 -> MPM")):
        if way in results:
            print(f"{name:14s} {label}: {fmt(results[way])}", flush=True)


def run_suite(cases, *, pwr: str | None = None, verbose: bool = False) -> int:
    """A suite's main: its cases (or a function of the MPM giving them) on the
    two devices, the results of each test name printed together. pwr: as
    run_case(), and set again at the end. Returns 1 if a packet was lost."""
    fails = 0
    mpm, cc = open_devices()
    with mpm, cc:
        name, r = None, {}
        for c in (cases(mpm) if callable(cases) else cases):
            if name is not None and c.name != name:
                print_results(name, r)
                fails += sum(v["good"] < v["count"] for v in r.values())
                r = {}
            r |= run_case(mpm, cc, c, pwr=pwr, verbose=verbose)
            name = c.name
        if name is not None:
            print_results(name, r)
            fails += sum(v["good"] < v["count"] for v in r.values())
        if pwr:
            cc.cmd(f"pwr {pwr}")
    return 1 if fails else 0


# The suites (suite_<family>.py) and their tests
FAMILIES = ("cc2500", "a7105", "cyrf", "nrf")


def suite(family: str):
    return importlib.import_module(f"suite_{family}")


def suite_names(family: str) -> list[str]:
    m = suite(family)
    return (list(m.NRF) if family == "nrf" else []) + list(m.P)


def suite_cases(family: str, names: list[str] | None = None, mpm: Bench | None = None,
                count: int | None = None, **kw) -> list[Case]:
    """The cases of a suite's tests (names: those of the family, None: all;
    none of them: no case). The CC2500 suite sets the MPM up for each
    (mpm). count: instead of the suite's; kw: the suite's cases() arguments."""
    if names:
        names = [n for n in names if n in suite_names(family)]
        if not names:
            return []
    m = suite(family)
    cs = list(m.cases(mpm, names, **kw) if family == "cc2500" else m.cases(names, **kw))
    if count:
        cs = [dataclasses.replace(c, kw=dict(c.kw, count=count)) for c in cs]
    return cs


@dataclass
class Setups:
    """A suite test's commands for one link, trim and channel left out: the
    MPM sending (mpm_tx) or receiving (mpm_rx), the CC2652 receiving (cc_rx)
    or sending (cc_tx, its power word first if the test has one)."""
    mpm_tx: list[str]
    mpm_rx: list[str]
    cc_rx: list[str]
    cc_tx: list[str]
    payload: bytes
    case: Case  # the first (period, channel: case.kw)


def suite_setups(family: str, name: str, mpm: Bench | None = None, **kw) -> Setups:
    """The commands of a suite test (suite_cases() arguments), as link_test()
    gives them to each direction"""
    cs = suite_cases(family, [name], mpm, **kw)
    if not cs:
        raise ValueError(f"no {family} test {name}")
    fwd = next((c for c in cs if c.kw.get("direction", "both") in ("both", "fwd")), cs[0])
    rev = next((c for c in cs if c.kw.get("direction", "both") in ("both", "rev")), cs[0])
    pwr = [f"pwr {cs[0].pwr}"] if cs[0].pwr else []
    return Setups(commands(fwd.mpm_setup), commands(rev.mpm_setup) + commands(rev.kw.get("mpm_rx_setup")),
                  pwr + commands(fwd.cc_setup), pwr + commands(rev.cc_setup) + commands(rev.kw.get("cc_rx_setup")),
                  cs[0].payload, cs[0])


def common_args(ap, period: int, count: int):
    ap.add_argument("--period", type=int, default=period, help=f"TX period in us (default {period})")
    ap.add_argument("--count", type=int, default=count, help=f"packets per direction (default {count})")
    ap.add_argument("--dir", default="both", choices=("both", "fwd", "rev"),
                    help="fwd: MPM -> CC2652, rev: CC2652 -> MPM")
    ap.add_argument("-v", "--verbose", action="store_true", help="print the setup commands")

