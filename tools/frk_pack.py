#!/usr/bin/env python3
"""A binary into a FrSky .frk file, as FrSky's S.Port boot loaders take it.

  frk_pack.py 0x8000 build-xjt/firmware.bin build-xjt/firmware.frk

The XJT-Lite's boot loader (CC2650) flashes .frk files at 0x8000 and up, sent
by the radio over S.Port (EdgeTX: flash an external module). Format from
github.com/raphaelcoeffic/frk_tools (packer.py, PROTOCOL.md): 2 KB blocks,
each with 1 KB of the image; a header (length, address, data offset) and the
scrambling seed at word 6; the 512 words XORed with a linear congruential
sequence from that seed, every word 2 of 16 also rotated by 7 bits; the
address minus a value of the seed. No FRSK header (the .frsk files' 16 bytes).
"""

import argparse
import struct

BLOCK = 1024             # image bytes per block
WORDS = 512              # 32-bit words per block (2 KB)
DATA_OFFSET = 0x13E      # the image bytes' offset in a block
SEED_WORD = 6            # the seed's word, not scrambled
SEED_START = 0x6569B868  # the first block's (FrSky's files)


def _next(m):
    # The boot loader replaces a 0 by 1 before each step
    return (0x7FF8A3ED * (m or 1) + 0x2AA01D31) & 0xFFFFFFFF


def _addr_key(seed):
    s = (seed & 0xFFFF) or 1
    return (0xAA * s % 0x7673) & 0xFFFF


def pack(image, addr):
    out = bytearray()
    seed = SEED_START
    for pos in range(0, len(image), BLOCK):
        data = image[pos:pos + BLOCK]
        # The boot loader writes only the block's length: no padding written
        block = bytearray(DATA_OFFSET) + data.ljust(BLOCK, b"\xFF") + bytearray(BLOCK - DATA_OFFSET)
        w = list(struct.unpack(f"<{WORDS}I", block))
        w[0] = len(data)
        w[1] = (addr + pos - _addr_key(seed)) & 0xFFFFFFFF
        w[3] = DATA_OFFSET - 32
        w[SEED_WORD] = seed
        m = seed
        for i in range(WORDS):
            m = _next(m)
            if i == SEED_WORD:
                continue
            v = w[i] ^ m
            if i & 0xF == 2:
                v = ((v << 25) | (v >> 7)) & 0xFFFFFFFF
            w[i] = v
        out += struct.pack(f"<{WORDS}I", *w)
        seed = m  # the next block's seed: where this one's sequence ended
    return bytes(out)


def main():
    p = argparse.ArgumentParser(description="A binary into a FrSky .frk file")
    p.add_argument("addr", type=lambda s: int(s, 0), help="flash address of the image (0x8000)")
    p.add_argument("bin", help="the image (objcopy -O binary)")
    p.add_argument("frk", help="the .frk file to write")
    a = p.parse_args()
    with open(a.bin, "rb") as f:
        image = f.read()
    with open(a.frk, "wb") as f:
        f.write(pack(image, a.addr))


if __name__ == "__main__":
    main()
