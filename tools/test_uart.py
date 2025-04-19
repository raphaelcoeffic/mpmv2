"""
Script that tests the receive timeout.
"""

import argparse
import serial
import sys
import time


def main(port: str):
    ser = serial.Serial(port, 921600)

    def test_packet(size: int) -> None:
        ser.write(bytes(b"\xaa") * size)
        time.sleep(0.1)

        b = ser.read()
        if b[0] != size:
            print(f"error: read mismatch (size={size} != {b[0]})")
            sys.exit(1)

        print(f"success: size = {size}")

    test_packet(11)
    test_packet(15)
    test_packet(20)
    test_packet(24)
    test_packet(26)
    test_packet(32)
    test_packet(36)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("port", help="serial port")
    args = parser.parse_args()
    main(args.port)
