#!/usr/bin/env python3
"""Fill in the CRCs of the CCFG (.ccfg section) of a CC27xx ELF file, in place.

Four CRC32 (zlib's) over the CCFG, each stored right after its range: boot
configuration 0x000..0x00C, the main part 0x010..0x74C, the user record
0x750..0x7CC (optional: only SACI's verify command checks it) and the debug
configuration 0x7D0..0x7FC. The boot ROM checks the other three.
"""

import argparse
import os
import struct
import subprocess
import tempfile
import zlib

CCFG_SIZE = 0x800
RANGES = [(0x000, 0x00C), (0x010, 0x74C), (0x750, 0x7CC), (0x7D0, 0x7FC)]


def fill_crcs(ccfg: bytearray) -> None:
    for start, end in RANGES:
        struct.pack_into("<I", ccfg, end, zlib.crc32(ccfg[start:end]))


def main() -> None:
    parser = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    parser.add_argument("elf")
    parser.add_argument("--objcopy", default="arm-none-eabi-objcopy")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "ccfg.bin")
        subprocess.run([args.objcopy, "--dump-section", f".ccfg={path}", args.elf], check=True)
        with open(path, "rb") as f:
            ccfg = bytearray(f.read())
        if len(ccfg) != CCFG_SIZE:
            raise SystemExit(f"{args.elf}: .ccfg is {len(ccfg)} bytes, not {CCFG_SIZE}")
        fill_crcs(ccfg)
        with open(path, "wb") as f:
            f.write(ccfg)
        subprocess.run([args.objcopy, "--update-section", f".ccfg={path}", args.elf], check=True)


if __name__ == "__main__":
    main()
