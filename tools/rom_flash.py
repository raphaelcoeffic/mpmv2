#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""First install of the MPM v2 over USB-C: the CC2652's ROM boot loader,
no debugger. Writes the whole flash (build/mpm-v2-full.bin: boot loader,
application, CCFG); later updates go through the MPM v2 boot loader instead
(the radio, or tools/stk500_flash.py).

  rom_flash.py [FILE]          erase, write, verify by CRC, reset
  rom_flash.py --info          the chip and the boot loader, nothing written

FILE: a binary of the whole flash from 0x0 (default: build/mpm-v2-full.bin).
The 0xFF (erased) ranges are not sent; the CCFG sector goes last, so an
interrupted write never leaves a CCFG pointing at a missing boot loader.

The ROM boot loader opens through the CH343: DTR resets the CC2652, RTS
holds BOOT (DIO19) low as the reset ends (CCFG's backdoor, kept by the MPM v2
boot loader). A blank chip opens it at any reset. Protocol: TI's CC13x2 /
CC26x2 technical reference manual (SWCU185), "ROM Bootloader".

Port: --port, else $V2_PORT, else the first CH343.
"""

import argparse
import os
import struct
import sys
import time
import zlib
from pathlib import Path

# pyserial: lib/pyserial (vendored, Python alone runs the script), else the
# installed one (uv run: the script's dependencies above)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "lib", "pyserial"))
import serial  # noqa: E402
from serial.tools import list_ports  # noqa: E402

CH343 = (0x1A86, 0x55D3)
ACK, NACK = b"\x00\xcc", b"\x00\x33"

PING, DOWNLOAD, GET_STATUS, SEND_DATA, RESET = 0x20, 0x21, 0x23, 0x24, 0x25
SECTOR_ERASE, CRC32, GET_CHIP_ID, MEMORY_READ, BANK_ERASE = 0x26, 0x27, 0x28, 0x2A, 0x2C
STATUS = {0x40: "success", 0x41: "unknown command", 0x42: "invalid command",
          0x43: "invalid address", 0x44: "flash failure"}

SECTOR = 0x2000
FLASH_SIZE_REG = 0x4003002C  # FLASH:FLASH_SIZE, 8 KB sectors
ICEPICK_DEVICE_ID = 0x50001318
DATA_MAX = 252  # SEND_DATA payload (the packet is 255 bytes at most)


class BlError(Exception):
    pass


def find_port() -> str:
    env = os.environ.get("V2_PORT")
    if env:
        return env
    ports = sorted(p.device for p in list_ports.comports() if (p.vid, p.pid) == CH343)
    if not ports:
        sys.exit("No MPM v2 (CH343) found: connect its USB-C, or give the port (--port, $V2_PORT; MULTI_FLASH_PORT in CMake)")
    return ports[0]


class RomBl:
    def __init__(self, port: str, baud: int):
        self.ser = serial.Serial(port, baud, timeout=1)

    def close(self):
        self.ser.close()

    def enter(self):
        """Reset with BOOT low (the backdoor), then autobaud"""
        # Q1 as on ESP32 boards: DTR low and RTS high reset, the other way
        # round BOOT low, both the same level nothing (pyserial: False = low)
        self.ser.rts = True
        self.ser.dtr = False
        time.sleep(0.1)
        self.ser.rts = False
        self.ser.dtr = True
        time.sleep(0.1)
        self.ser.rts = True
        time.sleep(0.1)
        self.ser.reset_input_buffer()
        self.ser.write(b"\x55\x55")
        r = self.ser.read(2)
        if r != ACK:
            raise BlError(f"no answer from the ROM boot loader ({r.hex() or 'nothing'}): "
                          "the CH343 lines, or another program on the port?")

    def leave(self):
        """Reset into the firmware (BOOT high)"""
        self.ser.dtr = False
        time.sleep(0.05)
        self.ser.rts = True
        self.ser.dtr = True

    def _ack(self, what: str):
        # 0x00 then 0xCC (ACK) or 0x33 (NACK), zeros allowed before
        while True:
            b = self.ser.read(1)
            if not b:
                raise BlError(f"{what}: no answer")
            if b != b"\x00":
                break
        if b == ACK[1:]:
            return
        raise BlError(f"{what}: {'NACK' if b == NACK[1:] else 'not an ACK (' + b.hex() + ')'}")

    def cmd(self, cmd: int, payload: bytes = b"", what: str = "") -> None:
        data = bytes([cmd]) + payload
        self.ser.write(bytes([len(data) + 2, sum(data) & 0xFF]) + data)
        self._ack(what or f"command 0x{cmd:02x}")

    def _response(self) -> bytes:
        while True:
            n = self.ser.read(1)
            if not n:
                raise BlError("no response packet")
            if n != b"\x00":
                break
        n = n[0]
        csum = self.ser.read(1)[0]
        data = self.ser.read(n - 2)
        if len(data) != n - 2 or sum(data) & 0xFF != csum:
            self.ser.write(NACK)
            raise BlError("bad response packet")
        self.ser.write(ACK)
        return data

    def status(self) -> int:
        self.cmd(GET_STATUS)
        return self._response()[0]

    def check(self, what: str):
        s = self.status()
        if s != 0x40:
            raise BlError(f"{what}: {STATUS.get(s, hex(s))}")

    def chip_id(self) -> int:
        self.cmd(GET_CHIP_ID)
        return struct.unpack(">I", self._response())[0]

    def read32(self, addr: int) -> int:
        self.cmd(MEMORY_READ, struct.pack(">IBB", addr, 1, 1))
        return struct.unpack("<I", self._response())[0]

    def crc32(self, addr: int, size: int) -> int:
        self.cmd(CRC32, struct.pack(">III", addr, size, 0))
        return struct.unpack(">I", self._response())[0]

    def erase_all(self, sectors: int):
        self.cmd(BANK_ERASE, what="bank erase")
        if self.status() == 0x40:
            return
        # bank erase off in CCFG: sector by sector
        for s in range(sectors):
            self.cmd(SECTOR_ERASE, struct.pack(">I", s * SECTOR), what=f"erase 0x{s * SECTOR:05x}")
            self.check(f"erase 0x{s * SECTOR:05x}")

    def write(self, addr: int, data: bytes, progress=None):
        self.cmd(DOWNLOAD, struct.pack(">II", addr, len(data)), what=f"download 0x{addr:05x}")
        self.check(f"download 0x{addr:05x}")
        for off in range(0, len(data), DATA_MAX):
            self.cmd(SEND_DATA, data[off:off + DATA_MAX], what=f"data 0x{addr + off:05x}")
            self.check(f"write 0x{addr + off:05x}")
            if progress:
                progress(len(data[off:off + DATA_MAX]))


def ranges(img: bytes, flash_size: int) -> list[tuple[int, bytes]]:
    """The data to write, sector by sector without the erased words at both
    ends; the CCFG sector last"""
    out = []
    for s in range(0, flash_size, SECTOR):
        chunk = img[s:s + SECTOR]
        words = [i for i in range(0, len(chunk), 4) if chunk[i:i + 4] != b"\xff\xff\xff\xff"]
        if words:
            a, b = words[0], words[-1] + 4
            out.append((s + a, chunk[a:b]))
    ccfg = [r for r in out if r[0] >= flash_size - SECTOR]
    return [r for r in out if r[0] < flash_size - SECTOR] + ccfg


def main():
    default = Path(__file__).resolve().parent.parent / "build" / "mpm-v2-full.bin"
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", nargs="?", default=str(default))
    ap.add_argument("-p", "--port")
    ap.add_argument("-b", "--baud", type=int, default=460800,
                    help="the ROM boot loader detects it (default 460800; 115200 if it fails)")
    ap.add_argument("--info", action="store_true", help="read the chip only")
    a = ap.parse_args()

    img = b"" if a.info else Path(a.file).read_bytes()
    bl = RomBl(a.port or find_port(), a.baud)
    try:
        bl.enter()
        chip = bl.chip_id()
        dev = bl.read32(ICEPICK_DEVICE_ID)
        sectors = bl.read32(FLASH_SIZE_REG) & 0xFF
        flash_size = sectors * SECTOR
        print(f"ROM boot loader: chip id {chip:08x}, ICEPick device id {dev:08x}, "
              f"flash {flash_size // 1024} KB", file=sys.stderr)
        if a.info:
            bl.leave()
            return
        if len(img) != flash_size:
            sys.exit(f"{a.file}: {len(img)} bytes, not the flash size ({flash_size}): "
                     "the whole flash from 0x0 (mpm-v2-full.bin) is expected")

        todo = ranges(img, flash_size)
        total = sum(len(d) for _, d in todo)
        t = time.monotonic()
        print("erasing...", file=sys.stderr)
        bl.erase_all(sectors)
        done = 0

        def progress(n):
            nonlocal done
            done += n
            if sys.stderr.isatty():
                print(f"\rwriting {done}/{total} bytes", end="", file=sys.stderr)

        for addr, data in todo:
            bl.write(addr, data, progress)
        if sys.stderr.isatty():
            print(file=sys.stderr)
        for addr, data in todo:
            crc = bl.crc32(addr, len(data))
            if crc != zlib.crc32(data):
                sys.exit(f"verify 0x{addr:05x}: CRC {crc:08x}, expected {zlib.crc32(data):08x}")
        print(f"{a.file}: {total} bytes written and verified in {time.monotonic() - t:.1f} s",
              file=sys.stderr)
        bl.cmd(RESET)
        bl.leave()
        print("reset: the MPM v2 boot loader starts the application", file=sys.stderr)
    except BlError as e:
        sys.exit(f"rom_flash: {e}")
    finally:
        bl.close()


if __name__ == "__main__":
    main()
