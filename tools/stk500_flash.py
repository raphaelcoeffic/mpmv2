#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""Flash an MPM v2 application through its boot loader, the way EdgeTX does
it (radio/src/io/multi_firmware_update.cpp): STK500v1 at 57600 8N1, 256 byte
pages from word address 0x1000, low 16 bits of the address only, last page
padded with zeros, LEAVE_PROGMODE at the end or on error.

  stk500_flash.py build-bl/mpm-v2.bin               USB-C (CH343): reset, sync
  stk500_flash.py build-bl/mpm-v2.bin --verify      read every page back
  stk500_flash.py any.bin --no-check                send a file EdgeTX would refuse
  stk500_flash.py --port /dev/cu.usbserial-X --no-reset --invert ...
                                                    radio bay through a USB
                                                    serial adapter
  stk500_flash.py --port /dev/cu.usbmodemX --radio ...
                                                    through the radio's USB
                                                    (EdgeTX CLI, external
                                                    module pass-through)

Port: --port, else $V2_PORT, else the first CH343. The CH343 resets the
CC2652 (DTR) with BOOT high (RTS), then the sync goes out every 10 ms: the
boot loader listens on the console for at least 50 ms after reset.

--radio: "serialpassthrough rfmod 1 57600" to the EdgeTX CLI, which stops
the pulses, powers the bay off for 500 ms and on again with the ports open
(EdgeTX patch, branch mpm-v2-passthrough-2.12): the sync goes out every
10 ms for the boot loader's 300 ms window. The radio stays in pass-through
until its USB cable is unplugged.
"""

import argparse
import os
import sys
import time

import serial
from serial.tools import list_ports

STK_OK, STK_FAILED, STK_INSYNC, CRC_EOP = 0x10, 0x11, 0x14, 0x20
GET_SYNC, LEAVE_PROGMODE, LOAD_ADDRESS, PROG_PAGE, READ_PAGE, READ_SIGN = 0x30, 0x51, 0x55, 0x64, 0x74, 0x75
CH343 = (0x1A86, 0x55D3)
SIG_LEN = 24
BOARD_CC2652 = 3


def find_port() -> str:
    if os.environ.get("V2_PORT"):
        return os.environ["V2_PORT"]
    for p in list_ports.comports():
        if (p.vid, p.pid) == CH343:
            return p.device
    raise SystemExit("No CH343 found, use --port")


def check_signature(data: bytes) -> str:
    """The checks EdgeTX does on the file (readV2Signature), board type 3."""
    sig = data[-SIG_LEN:]
    if not sig.startswith(b"multi-x"):
        raise SystemExit("no multi-x signature at the end of the file")
    flags = int(sig[7:15], 16)
    if flags & 3 != BOARD_CC2652 or not flags & 0x80 or not flags & 0x100 or not flags & 0x800:
        raise SystemExit(f"signature flags {flags:08x}: not an MPM v2 application")
    return sig.decode()


class Stk:
    def __init__(self, port: str, invert: bool):
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, 57600, 0.1
        self.s.dtr = self.s.rts = True  # CH343: run, BOOT high
        self.s.open()
        self.invert = invert

    def tx(self, *data: int):
        b = bytes(data)
        self.s.write(bytes(x ^ 0xFF for x in b) if self.invert else b)

    def rx(self, timeout: float = 0.1) -> int | None:
        self.s.timeout = timeout
        b = self.s.read(1)
        if not b:
            return None
        return b[0] ^ 0xFF if self.invert else b[0]

    def reset(self):
        self.s.dtr = False
        time.sleep(0.05)
        self.s.dtr = True

    def passthrough(self):
        self.s.reset_input_buffer()
        self.s.write(b"serialpassthrough rfmod 1 57600\n")
        time.sleep(0.2)
        print(self.s.read(self.s.in_waiting).decode(errors="replace").strip())

    def sync(self, period: float, timeout: float) -> bool:
        self.s.reset_input_buffer()
        end = time.time() + timeout
        while time.time() < end:
            self.tx(GET_SYNC, CRC_EOP)
            if self.rx(period) == STK_INSYNC and self.rx() == STK_OK:
                time.sleep(0.05)
                self.s.reset_input_buffer()  # the replies to the other syncs
                return True
        return False

    def cmd(self, *data: int, n: int = 0) -> bytes:
        self.s.reset_input_buffer()
        self.tx(*data, CRC_EOP)
        if self.rx() != STK_INSYNC:
            raise RuntimeError(f"command {data[0]:02x}: no INSYNC")
        out = bytes(self.rx() or 0 for _ in range(n))
        if self.rx() != STK_OK:
            raise RuntimeError(f"command {data[0]:02x}: no OK")
        return out

    def prog_page(self, page: bytes) -> int | None:
        self.s.reset_input_buffer()
        self.tx(PROG_PAGE, len(page) >> 8, len(page) & 0xFF, 0, *page, CRC_EOP)
        if self.rx() != STK_INSYNC:
            raise RuntimeError("PROG_PAGE: no INSYNC")
        for _ in range(4):  # EdgeTX: 4 tries of 100 ms, zeros skipped
            b = self.rx()
            if b:
                return b
        return None

    def read_page(self, n: int) -> bytes:
        self.s.reset_input_buffer()
        self.tx(READ_PAGE, n >> 8, n & 0xFF, ord("F"), CRC_EOP)
        if self.rx() != STK_INSYNC:
            raise RuntimeError("READ_PAGE: no INSYNC")
        self.s.timeout = 0.5
        data = self.s.read(n)
        if self.invert:
            data = bytes(x ^ 0xFF for x in data)
        if len(data) != n or self.rx() != STK_OK:
            raise RuntimeError("READ_PAGE: short reply")
        return data

    def leave(self):
        self.tx(LEAVE_PROGMODE, CRC_EOP)
        self.rx()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", help="mpm-v2.bin (image + MULTI signature)")
    ap.add_argument("--port")
    ap.add_argument("--no-reset", action="store_true", help="don't reset through DTR, sync for 5 s as EdgeTX")
    ap.add_argument("--radio", action="store_true", help="EdgeTX CLI on --port: pass-through to the external bay")
    ap.add_argument("--invert", action="store_true", help="inverted serial (radio bay through a plain adapter)")
    ap.add_argument("--no-check", action="store_true", help="skip the file signature check")
    ap.add_argument("--verify", action="store_true", help="read each page back after writing it")
    a = ap.parse_args()

    data = open(a.file, "rb").read()
    if not a.no_check:
        print(f"signature {check_signature(data)}")

    stk = Stk(a.port or find_port(), a.invert)
    if a.radio:
        stk.passthrough()
        ok = stk.sync(0.01, 2)
    elif a.no_reset:
        ok = stk.sync(0.1, 5)
    else:
        stk.reset()
        ok = stk.sync(0.01, 1)
    if not ok:
        raise SystemExit("no sync")

    sig = stk.cmd(READ_SIGN, n=3)
    print(f"device signature {sig.hex()}")
    if sig[0] != 0x1E:
        stk.leave()
        raise SystemExit("not an STK500 device")
    page_size, word = (256, 0x1000) if sig[1] == 0x55 and sig[2] in (0xAA, 0x26) else (128, 0)

    t0 = time.time()
    err = None
    n = 0
    for off in range(0, len(data), page_size):
        page = data[off:off + page_size].ljust(page_size, b"\0")
        stk.cmd(LOAD_ADDRESS, word & 0xFF, (word >> 8) & 0xFF)
        r = stk.prog_page(page)
        if r != STK_OK:
            err = f"page at +0x{off:x}: {'STK_FAILED' if r == STK_FAILED else r}"
            break
        # Read back at once: the boot loader follows the 16 bit address
        # forward only (past 128 KB), as EdgeTX sends it
        if a.verify and stk.read_page(page_size) != page:
            err = f"verify: page at +0x{off:x} differs"
            break
        n += 1
        word += page_size // 2
        print(f"\r{off + page_size}/{len(data)} bytes", end="", flush=True)
    print(f" in {time.time() - t0:.1f} s" + (f", {n} pages verified" if a.verify else ""))

    stk.leave()
    if err:
        raise SystemExit(err)
    print("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
