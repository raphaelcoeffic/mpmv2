"""
Script that triggers the built-in bootloader backdoor.
"""

import argparse
import serial
import time

ACK = b"\x00\xcc"


def main(port: str):
    ser = serial.Serial(port, 115200)

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


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="serial port")
    args = parser.parse_args()
    main(args.port)
