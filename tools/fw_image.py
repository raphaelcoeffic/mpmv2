#!/usr/bin/env python3
"""Application image for the MPM v2 boot loader (docs/update.md).

  fw_image.py build/firmware.bin --version 0.1.0.0 --out build/mpm-v2.bin \
      --full build/bootloader.bin build/mpm-v2-full

Fills in the image header the boot loader checks at every start (in place:
the file the J-Link flashes at 0x2000), and writes the file for the radio:
the same image followed by the 24 byte MULTI signature EdgeTX reads,
multi-x<flags>-<version>. With --full, the whole flash for a first install
(a debugger, or the ROM boot loader over USB): the boot loader and CCFG
(objcopy -O binary --gap-fill 0xff of bootloader.elf: 0x0 to the flash
end), the application at 0x2000, as PREFIX.bin and PREFIX.hex.

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
FLASH_SIZE = 0x58000

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


def ihex(data: bytes) -> str:
    """Intel HEX of data at 0, the erased (0xFF) 16 byte rows left out"""
    lines, base = [], -1

    def rec(t: int, addr: int, payload: bytes):
        r = bytes([len(payload), addr >> 8, addr & 0xFF, t]) + payload
        lines.append(":" + (r + bytes([-sum(r) & 0xFF])).hex().upper())

    for off in range(0, len(data), 16):
        row = data[off:off + 16]
        if row == b"\xff" * len(row):
            continue
        if off >> 16 != base:
            base = off >> 16
            rec(4, 0, struct.pack(">H", base))
        rec(0, off & 0xFFFF, row)
    rec(1, 0, b"")
    return "\n".join(lines) + "\n"


def full_image(bootloader: bytes, app: bytes) -> bytes:
    if len(bootloader) != FLASH_SIZE:
        sys.exit(f"boot loader binary: {len(bootloader)} bytes, not 0x{FLASH_SIZE:x} "
                 "(objcopy --gap-fill 0xff, CCFG at the end)")
    if bootloader[APP_START:APP_END] != b"\xff" * (APP_END - APP_START):
        sys.exit("boot loader binary: data in the application range")
    return bootloader[:APP_START] + app + b"\xff" * (APP_END - APP_START - len(app)) + bootloader[APP_END:]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bin", help="application binary linked at 0x2000 (objcopy -O binary)")
    ap.add_argument("--version", default="0.0.0.0", help="a.b.c.d")
    ap.add_argument("--out", help="file for the radio (image + MULTI signature)")
    ap.add_argument("--full", nargs=2, metavar=("BOOTLOADER_BIN", "PREFIX"),
                    help="the whole flash: PREFIX.bin and PREFIX.hex")
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
    if a.full:
        full = full_image(open(a.full[0], "rb").read(), bytes(img))
        open(a.full[1] + ".bin", "wb").write(full)
        open(a.full[1] + ".hex", "w").write(ihex(full))
    length, crc = struct.unpack_from("<II", img, HDR_OFFSET + 4)
    print(f"{a.bin}: {length} bytes, crc32 {crc:08x}" + (f", {a.out}: {signature(a.version).decode()}" if a.out else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
