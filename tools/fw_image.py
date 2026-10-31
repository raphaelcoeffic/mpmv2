#!/usr/bin/env python3
"""File for the radio from an MPM v2 application (docs/update.md).

  fw_image.py build/firmware.bin --version 0.1.0.0 --out build/mpm-v2.bin

The application binary (objcopy -O binary) has its image header right after
its vector table (src/core/app_image.h, found by its magic as the boot
loader does): magic, chip, length, no CRC, `checked` already 0, so a
debugger or the ROM boot loader can write it as it is. The radio's file gets
the CRC32 and `checked` 0xFFFFFFFF (the boot loader checks the CRC at the
first start after an update), followed by the 24 byte MULTI signature EdgeTX
reads, multi-x<flags>-<version>.

Layout: src/boards/mpm_v2/bootloader/layout.h.
"""

import argparse
import struct
import sys
import zlib

APP_START = 0x2000
HDR_MAGIC = 0x494D504D  # "MPMI"
HDR_MIN, HDR_SEARCH = 0x40, 0x200
HDR = struct.Struct("<IIIII")  # magic, chip, length, crc, checked
# Chip (PLATFORM_CHIP_ID): the application's end (APP_END), its SRAM
CHIPS = {0x2652: (0x56000, (0x20000000, 0x20014000))}

# MULTI signature flags (Multiprotocol/Binary_Signature.ino): module type
# 3 (CC2652, new), boot loader support, CHECK_FOR_BOOTLOADER, MULTI_TELEMETRY
BOARD_CC2652 = 3
FLAGS = BOARD_CC2652 | 0x80 | 0x100 | 0x800


def header_offset(img: bytes) -> int:
    """The header's offset (app_header_offset()), 0 if none"""
    for off in range(HDR_MIN, min(len(img), HDR_SEARCH) - HDR.size + 1, 4):
        if struct.unpack_from("<I", img, off)[0] == HDR_MAGIC:
            return off
    return 0


def header(img: bytes, off: int, chip: int) -> bytes:
    """magic, chip, length, CRC32 of the image with the header read as
    zeros, `checked` not yet (the boot loader clears it once the CRC matched)"""
    body = img[:off] + bytes(HDR.size) + img[off + HDR.size:]
    return HDR.pack(HDR_MAGIC, chip, len(img), zlib.crc32(body), 0xFFFFFFFF)


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
    ap.add_argument("--out", required=True, help="file for the radio (image + MULTI signature)")
    a = ap.parse_args()

    img = bytearray(open(a.bin, "rb").read())
    off = header_offset(img)
    if not off:
        sys.exit(f"{a.bin}: no image header (src/core/app_header.c)")
    _, chip, length, _, _ = HDR.unpack_from(img, off)
    if chip not in CHIPS:
        sys.exit(f"{a.bin}: chip 0x{chip:x}, not one of {', '.join(f'0x{c:x}' for c in CHIPS)}")
    app_end, sram = CHIPS[chip]
    sp, pc = struct.unpack_from("<II", img)
    if not (sram[0] < sp <= sram[1]) or not (APP_START < (pc & ~1) < app_end):
        sys.exit(f"{a.bin}: not linked at 0x{APP_START:x} (sp 0x{sp:08x}, reset 0x{pc:08x})")
    if length != len(img):
        sys.exit(f"{a.bin}: header length {length} for {len(img)} bytes")
    if APP_START + len(img) > app_end:
        sys.exit(f"{a.bin}: {len(img)} bytes, more than {app_end - APP_START}")

    img[off:off + HDR.size] = header(bytes(img), off, chip)
    open(a.out, "wb").write(bytes(img) + signature(a.version))
    crc = HDR.unpack_from(img, off)[3]
    print(f"{a.out}: {len(img)} bytes, chip 0x{chip:x}, header at 0x{off:x}, "
          f"crc32 {crc:08x}, {signature(a.version).decode()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
