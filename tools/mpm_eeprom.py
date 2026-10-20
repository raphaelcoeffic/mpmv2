#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# ///
"""Decode the MULTI-Module EEPROM: the MPM v2's "eeprom" file (2 KB) or a
dump of an STM32 MULTI-Module's, in the layout of Multiprotocol.h
(v1.3.4.74, "EEPROM Layout"). Only what is set is printed (an erased byte
is 0xFF); --all prints the erased entries too.

  mpm_eeprom.py FILE [--all]
  mpm_fs.py eeprom [FILE] [--all]      from the module over USB-C, or FILE
"""

import argparse

SIZE = 2048
FLAG = 0xF0  # "initialised" flags
# FrSky RX formats (FrSky_Rx_cc2500.ino), also those of the clone entries
FRSKY_FORMATS = {0: "D8", 1: "D16 FCC", 2: "D16 LBT", 3: "D16v2 FCC", 4: "D16v2 LBT"}


def _hex(b: bytes) -> str:
    return b.hex()


def _erased(b: bytes) -> bool:
    return all(x == 0xFF for x in b)


def _per_model(name: str, data: bytes, offset: int, size: int, count: int, fmt=_hex, base: int = 0):
    """A table with one entry per model (rx_num)"""
    out = []
    for i in range(count):
        e = data[offset + i * size:offset + (i + 1) * size]
        out.append((f"{name} rx_num {base + i}", e, fmt(e)))
    return out


def _frsky_rx(e: bytes) -> str:
    # format, TX ID (rx_tx_addr[3], [2], [1]), freq_tune, 47 hop channels
    tune = e[4] - 256 if e[4] > 127 else e[4]
    return f"format {e[0]} ({FRSKY_FORMATS.get(e[0], '?')}), TX ID {_hex(e[1:4])}, freq_tune {tune}, hop {_hex(e[5:])}"


def _frsky_clone(e: bytes) -> str:
    # format, TX ID (rx_tx_addr[3], [2], [1]), 47 hop channels (D and X only)
    s = f"format {e[0]} ({FRSKY_FORMATS.get(e[0], '?')}), TX ID {_hex(e[1:4])}"
    return s + (f", hop {_hex(e[4:])}" if len(e) > 4 else "")


def decode(data: bytes, all_entries: bool = False) -> list[str]:
    data = data[:SIZE].ljust(SIZE, b"\xff")
    entries = []  # (name, raw bytes, text)

    cyrf = data[1:7]
    entries.append(("CYRF ID", data[0:7], f"{_hex(cyrf)}" + ("" if data[0] == FLAG else
                                                            f" (flag {data[0]:02x}: not set)")))
    mid = data[10:14]
    entries.append(("module ID", data[10:14] + data[20:21],
                    f"{int.from_bytes(mid, 'little'):08x}" + ("" if data[20] == FLAG else
                                                              f" (valid flag {data[20]:02x}: not set)")))
    entries.append(("dial bank", data[15:16], f"{data[15]}"))
    entries += _per_model("autobind mode (Devo, WK2x01)", data, 30, 1, 16,
                          lambda e: {0: "autobind", 1: "fixed ID"}.get(e[0], f"{e[0]:02x}"))
    entries += _per_model("AFHDS2A RX ID", data, 50, 4, 16)
    entries += _per_model("AFHDS2A RX ID", data, 250, 4, 48, base=16)
    entries += _per_model("Bugs RX ID", data, 114, 2, 16)
    entries += _per_model("BugsMini RX ID", data, 146, 2, 16)
    entries.append(("FrSky RX", data[178:230], _frsky_rx(data[178:230])))
    entries.append(("AFHDS2A RX", data[230:250], f"TX ID {_hex(data[230:234])}, channels {_hex(data[234:250])}"))
    entries += _per_model("HoTT RX ID", data, 442, 5, 64)
    entries.append(("Bayang RX", data[762:771], f"TX ID {_hex(data[762:767])}, channels {_hex(data[767:771])}"))
    entries.append(("FrSky D clone", data[771:822], _frsky_clone(data[771:822])))
    entries.append(("FrSky X clone", data[822:873], _frsky_clone(data[822:873])))
    entries.append(("FrSky X2 clone", data[873:877], _frsky_clone(data[873:877])))
    entries.append(("DSM RX", data[877:882], f"TX ID {_hex(data[877:881])}, format {data[881]:02x}"))
    entries += _per_model("MouldKg RX ID", data, 882, 3, 64)
    entries.append(("DSM clone", data[1074:1079], f"TX ID {_hex(data[1074:1078])}" +
                    ("" if data[1078] == FLAG else f" (flag {data[1078]:02x}: not set)")))
    entries += _per_model("Traxxas RX ID, SOP index", data, 1079, 3, 64)
    entries += _per_model("XK2 RX ID checksum", data, 1271, 1, 64)
    entries += _per_model("JiaBaiLe RX ID", data, 1335, 3, 64)

    lines = [f"{name}: {text}" if not _erased(raw) else f"{name}: -"
             for name, raw, text in entries if all_entries or not _erased(raw)]
    rest = data[1527:]
    if not _erased(rest):
        used = [i + 1527 for i, x in enumerate(rest) if x != 0xFF]
        lines.append(f"outside the layout (1527-2047): {len(used)} bytes set, from {used[0]}")
    return lines or ["(erased)"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--all", action="store_true", help="the erased entries too")
    a = ap.parse_args()
    with open(a.file, "rb") as f:
        print("\n".join(decode(f.read(), a.all)))


if __name__ == "__main__":
    main()
