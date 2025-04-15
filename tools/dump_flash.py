"""
Script that dumps the external flash to stdout (Intel HEX).
"""

import serial
import sys

BAUDRATE = 921600
DUMP_CMD = b"dump_flash"

# see https://en.wikipedia.org/wiki/Intel_HEX
HEX_EOF = b":00000001FF\n"

if len(sys.argv) < 2:
    print("Missing argument: serial port", file=sys.stderr)
    sys.exit(1)

ser = serial.Serial(sys.argv[1], BAUDRATE, timeout=1)

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
