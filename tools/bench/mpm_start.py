#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Start the firmware of the 4-in-1 MULTI-Module waiting in its USB boot
loader (STM32duino, "Maple 003", 1eaf:0003), nothing written.

The boot loader stays there at every power up when it sees a USB host (D-,
PA11, low), external 5V or not. Its DFU code (STM32duino-bootloader dfu.c)
sets the "just uploaded" boot flag at the first DFU_DNLOAD with data and
copies that data to flash only at the next DFU_GETSTATUS. So: DNLOAD of one
byte on alt 2 (0x8002000), then a USB reset: the chip resets and the boot
loader starts the firmware at once. The bench firmware then shows up as
"Maple" (1eaf:0004). Same as multi_flash.py --method start in the MULTI
repository (cmake/), kept here so the bench does not need it.

  mpm_start.py

Needs libusb-1.0 (Homebrew: libusb, dfu-util's).
"""

import ctypes
import ctypes.util
import sys
import time

VID, PID_DFU = 0x1EAF, 0x0003
DFU_DNLOAD = 1


def libusb():
    for name in (ctypes.util.find_library("usb-1.0"), "/opt/homebrew/lib/libusb-1.0.dylib",
                 "/usr/local/lib/libusb-1.0.dylib", "libusb-1.0.so.0"):
        if not name:
            continue
        try:
            lib = ctypes.CDLL(name)
        except OSError:
            continue
        vp, i = ctypes.c_void_p, ctypes.c_int
        lib.libusb_init.argtypes = [ctypes.POINTER(vp)]
        lib.libusb_exit.argtypes = [vp]
        lib.libusb_open_device_with_vid_pid.argtypes = [vp, ctypes.c_uint16, ctypes.c_uint16]
        lib.libusb_open_device_with_vid_pid.restype = vp
        lib.libusb_claim_interface.argtypes = [vp, i]
        lib.libusb_set_interface_alt_setting.argtypes = [vp, i, i]
        lib.libusb_control_transfer.argtypes = [vp, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint16,
                                                ctypes.c_uint16, ctypes.c_char_p, ctypes.c_uint16, ctypes.c_uint]
        lib.libusb_reset_device.argtypes = [vp]
        lib.libusb_close.argtypes = [vp]
        lib.libusb_error_name.argtypes = [i]
        lib.libusb_error_name.restype = ctypes.c_char_p
        return lib
    sys.exit("libusb-1.0 not found (brew install libusb)")


def main() -> int:
    lib = libusb()
    ctx = ctypes.c_void_p()
    if lib.libusb_init(ctypes.byref(ctx)) < 0:
        sys.exit("libusb_init failed")
    try:
        h = lib.libusb_open_device_with_vid_pid(ctx, VID, PID_DFU)
        if not h:
            print("no 4-in-1 in its boot loader (1eaf:0003): not connected, or its firmware runs already")
            return 1
        r = lib.libusb_claim_interface(h, 0)
        if r >= 0:
            r = lib.libusb_set_interface_alt_setting(h, 0, 2)
        if r >= 0:
            lib.libusb_control_transfer(h, 0x21, DFU_DNLOAD, 0, 0, b"\xff", 1, 1000)
        if r < 0:
            lib.libusb_close(h)
            print(f"cannot open the boot loader: {lib.libusb_error_name(r).decode()}")
            return 1
        lib.libusb_reset_device(h)  # the device goes away: an error is expected
        lib.libusb_close(h)
        time.sleep(2.5)  # a boot loader that stayed is back by then
        h = lib.libusb_open_device_with_vid_pid(ctx, VID, PID_DFU)
        if h:
            lib.libusb_close(h)
            print("still in the boot loader")
            return 1
        print("firmware started")
        return 0
    finally:
        lib.libusb_exit(ctx)


if __name__ == "__main__":
    sys.exit(main())
