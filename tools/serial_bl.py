"""
Script that triggers the built-in bootloader backdoor.
"""

import serial
import sys
import time

ACK = b"\x00\xcc"


ser = serial.Serial(sys.argv[1], 115200)


def set_reset_pin(value: bool) -> None:
    ser.dtr = value


def set_boot_pin(value: bool) -> None:
    ser.rts = value


def expect_ack():
    reply = ser.read(len(ACK))
    if reply != ACK:
        raise Exception(f"received {reply} instead of ACK")


# Trigger bootloader backdoor
#
# RESET LOW / BOOT HIGH
set_reset_pin(False)
time.sleep(0.1)
#
# RESET HIGH / BOOT LOW
set_boot_pin(False)
set_reset_pin(True)
time.sleep(0.1)
#
# RESET HIGH / BOOT HIGH
ser.rts = True
time.sleep(0.1)

# Send baudrate detection sequence
ser.write(b"\x55\x55")

# Wait for ACK
expect_ack()
print("success: received ACK")
