"""
Script that dumps the external flash to stdout (Intel HEX).
"""

import argparse
import serial
import sys

BAUDRATE = 921600
DUMP_CMD = b"dump_flash"

# see https://en.wikipedia.org/wiki/Intel_HEX
HEX_EOF = b":00000001FF\n"


def main(port: str):
    ser = serial.Serial(port, BAUDRATE, timeout=5)

    # flush buffer
    ser.read_all()

    # send command
    ser.write(b"dump_flash")

    while True:
        data = ser.readline()
        if not data:
            print("Timeout waiting for data", file=sys.stderr)
            sys.exit(1)

        print(data.decode(), end="")

        # detect EOF
        if data == HEX_EOF:
            print("Done", file=sys.stderr)
            sys.exit(0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="serial port")
    args = parser.parse_args()
    main(args.port)
