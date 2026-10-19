#!/usr/bin/env python3
"""Application image for the MPM v2 boot loader (docs/update.md).

  fw_image.py build/firmware.bin --version 0.1.0.0 --out build/mpm-v2.bin

Fills in the image header the boot loader checks at every start (in place:
the file the J-Link flashes at 0x2000), and writes the file for the radio:
the same image followed by the 24 byte MULTI signature EdgeTX reads,
multi-x<flags>-<version>.

Layout and header: src/bootloader/layout.h.
"""

import argparse
import struct
import sys
import zlib

APP_START = 0x2000
APP_END = 0x56000
HDR_OFFSET = 0x1C
HDR_MAGIC = 0x3256504D  # "MPV2"
HDR_LEN = 16
SRAM = (0x20000000, 0x20014000)

# MULTI signature flags (Multiprotocol/Binary_Signature.ino): module type
# 3 (CC2652, new), boot loader support, CHECK_FOR_BOOTLOADER, MULTI_TELEMETRY
BOARD_CC2652 = 3
FLAGS = BOARD_CC2652 | 0x80 | 0x100 | 0x800


def header(img: bytes) -> bytes:
    """magic, length, CRC32 of the image with the header read as zeros,
    `checked` not yet (the boot loader clears it once the CRC matched)"""
    body = img[:HDR_OFFSET] + bytes(HDR_LEN) + img[HDR_OFFSET + HDR_LEN:]
    return struct.pack("<IIII", HDR_MAGIC, len(img), zlib.crc32(body), 0xFFFFFFFF)


def signature(version: str) -> bytes:
    parts = [int(p) for p in version.split(".")]
    parts += [0] * (4 - len(parts))
    sig = f"multi-x{FLAGS:08x}-" + "".join(f"{p:02d}" for p in parts[:4])
    assert len(sig) == 24, sig
    return sig.encode()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bin", help="application binary linked at 0x2000 (objcopy -O binary)")
    ap.add_argument("--version", default="0.0.0.0", help="a.b.c.d")
    ap.add_argument("--out", help="file for the radio (image + MULTI signature)")
    a = ap.parse_args()

    img = bytearray(open(a.bin, "rb").read())
    sp, pc = struct.unpack_from("<II", img)
    if not (SRAM[0] < sp <= SRAM[1]) or not (APP_START < (pc & ~1) < APP_END):
        sys.exit(f"{a.bin}: not linked at 0x{APP_START:x} (sp 0x{sp:08x}, reset 0x{pc:08x})")
    if any(img[HDR_OFFSET:HDR_OFFSET + HDR_LEN]) and \
            struct.unpack_from("<I", img, HDR_OFFSET)[0] != HDR_MAGIC:
        sys.exit(f"{a.bin}: vector table entries 7 to 10 are in use")
    if APP_START + len(img) > APP_END:
        sys.exit(f"{a.bin}: {len(img)} bytes, more than {APP_END - APP_START}")

    img[HDR_OFFSET:HDR_OFFSET + HDR_LEN] = header(bytes(img))
    open(a.bin, "wb").write(img)
    if a.out:
        open(a.out, "wb").write(bytes(img) + signature(a.version))
    length, crc = struct.unpack_from("<II", img, HDR_OFFSET + 4)
    print(f"{a.bin}: {length} bytes, crc32 {crc:08x}" + (f", {a.out}: {signature(a.version).decode()}" if a.out else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
