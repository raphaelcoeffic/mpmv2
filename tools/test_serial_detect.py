"""
Script that tests the serial input detection.
"""

import argparse
import time

from serial import Serial, PARITY_EVEN

HDR = b"\x55"
PROT = b"\x01"
SUB = b"\x00"
OPT = b"\x00"

CHANNELS = [
    1024,
    1024,
    204,
    1024,
    1024,
    1024,
    1024,
    1024,
    1024,
    1024,
    1024,
    1024,
    1024,
    1024,
    1024,
    1024,
]

EXT = b"\x00"


def encode_channels() -> bytes:
    out = bytearray()
    reg = 0
    bits = 0

    for c in CHANNELS:
        reg = (reg << 11) | c
        bits += 11

        while bits >= 8:
            bits -= 8
            val = (reg >> bits) & 0xFF
            reg = reg & ((1 << bits) - 1)
            out.append(val)

    return bytes(out)


def build_packet() -> bytes:
    return HDR + PROT + SUB + OPT + encode_channels() + EXT


def main(port: str, flood: bool):
    ser = Serial(port, 100000, parity=PARITY_EVEN, stopbits=2)
    pkt = build_packet()

    while True:
        ser.write(pkt)
        ser.flush()

        # while ser.out_waiting > 0:
        #     pass

        # if not flood:
        time.sleep(0.01)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="serial port")
    parser.add_argument(
        "--flood",
        help="do not wait between packets",
        action=argparse.BooleanOptionalAction,
    )
    args = parser.parse_args()

    try:
        main(args.port, args.flood)
    except KeyboardInterrupt:
        print("done")
