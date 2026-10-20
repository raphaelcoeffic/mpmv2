# Servo output monitor, MicroPython on an RP2040-Zero (tools/bench/servo_mon.py):
# a receiver's servo output on GP12 (through a series resistor), its supply
# switched by GP11 (high-side switch). A second input on GP9 (also through a
# series resistor): another servo output, or SBUS.
#
# Commands (lines on the USB serial):
#   pwr 0|1               receiver supply (GP11 level), "ok pwr=N"
#   mon 0|1               pulse report on/off, "ok mon=N"
#   in2 off|servo|sbus [pin]
#                         second input, GP9 by default, "ok in2=X". GP13 is
#                         tied to GP12 on the bench (S.Port uploader wiring):
#                         SBUS on GP13 reads GP12's wire, GP12's pulse report
#                         stops meanwhile.
# Reports: "p <high> <period>" per GP12 pulse in 0.1 us units, "o <n>" when n
# pulses were lost (FIFO full); the second input as servo: "q <high> <period>"
# the same;
# as SBUS (100000 baud 8E2, inverted): "s <flags> <ch1> ... <ch16>" per frame,
# the flags byte and the 11-bit channel values in decimal; "x <n>" when n bytes
# were skipped to find a frame again.
#
# SBUS is received by a PIO state machine, not the UART: MicroPython's UART
# interrupt fires endlessly once its RX buffer is full (it stops reading the
# hardware FIFO), which hung the board. DMA moves the state machine's words
# into a 1 KB ring: the loop may lag 0.36 s (a garbage collection, a slow
# print) without losing bytes, where the FIFO alone holds 32.

import rp2
import select
import sys
import uctypes
from machine import Pin, mem32

SIG, PWR, SIG2 = 12, 11, 9
IO_BANK0 = 0x40014000  # GPIOn_CTRL at + 8 * n + 4, INOVER in bits 17:16
SBUS_SM = 2  # PIO0 SM2: its RX FIFO and DREQ
SBUS_RXF = 0x50200020 + 4 * SBUS_SM
SBUS_DREQ = 4 + SBUS_SM
RING_BITS = 10
RING = 1 << RING_BITS


# 10 MHz, two instructions per count: 0.2 us per count, the high and low
# times pushed per pulse (each counted down from 0xFFFFFFFF). The edge to
# edge overhead (the pushes, 5 instructions) is counted in neither.
@rp2.asm_pio(fifo_join=rp2.PIO.JOIN_RX)
def pulse():
    wait(0, pin, 0)
    wait(1, pin, 0)
    label("start")
    mov(x, invert(null))
    label("high")
    jmp(x_dec, "h2")
    label("h2")
    jmp(pin, "high")
    mov(y, invert(null))
    label("low")
    jmp(pin, "done")
    jmp(y_dec, "low")
    label("done")
    mov(isr, invert(x))
    push(noblock)
    mov(isr, invert(y))
    push(noblock)
    jmp("start")


# SBUS, 100000 baud 8E2, the input inverted in the GPIO block (idle high
# then). 8 cycles per bit: the data bits sampled from 1.4 bits after the start
# edge, LSB first; the parity bit skipped (a low one waited out), the stop
# bits are the idle wait for the next start. 4 bytes per FIFO word, the first
# in the low byte.
@rp2.asm_pio(in_shiftdir=rp2.PIO.SHIFT_RIGHT, autopush=True, push_thresh=32, fifo_join=rp2.PIO.JOIN_RX)
def sbus_rx():
    wait(0, pin, 0)
    set(x, 7)[10]
    label("bit")
    in_(pins, 1)[6]
    jmp(x_dec, "bit")
    wait(1, pin, 0)


sm = rp2.StateMachine(0, pulse, freq=10_000_000, in_base=Pin(SIG, Pin.IN), jmp_pin=Pin(SIG))
sm.active(1)
sm2 = None
sbus = None
sbus_buf = b""
# the DMA ring wraps on an address aligned to its size
ring_raw = bytearray(2 * RING)
ring_addr = (uctypes.addressof(ring_raw) + RING - 1) & ~(RING - 1)
ring = memoryview(ring_raw)[ring_addr - uctypes.addressof(ring_raw):][:RING]
ring_rd = 0
dma = None
in2_pin = SIG2
pwr = None
mon = True
poll = select.poll()
poll.register(sys.stdin, select.POLLIN)
line = ""


def invert(pin, on):
    a = IO_BANK0 + 8 * pin + 4
    mem32[a] = (mem32[a] & ~(3 << 16)) | (on << 16)


def in2(mode, pin):
    global sm2, sbus, sbus_buf, in2_pin, dma, ring_rd
    if sm2:
        sm2.active(0)
        sm2 = None
    if sbus:
        sbus.active(0)
        sbus = None
        dma.close()
        dma = None
    Pin(in2_pin, Pin.IN)
    invert(in2_pin, 0)
    in2_pin = pin
    shared = mode == "sbus" and pin == SIG + 1
    sm.active(0)
    if not shared:
        sm.restart()
        while sm.rx_fifo():
            sm.get()
        sm.active(1)
    if mode == "servo":
        sm2 = rp2.StateMachine(1, pulse, freq=10_000_000, in_base=Pin(pin, Pin.IN), jmp_pin=Pin(pin))
        sm2.active(1)
    elif mode == "sbus":
        invert(pin, 1)
        sbus = rp2.StateMachine(SBUS_SM, sbus_rx, freq=800_000, in_base=Pin(pin))
        dma = rp2.DMA()
        ctrl = dma.pack_ctrl(size=2, inc_read=False, ring_size=RING_BITS, ring_sel=True, treq_sel=SBUS_DREQ)
        dma.config(read=SBUS_RXF, write=ring_addr, count=0xFFFFFFFF, ctrl=ctrl, trigger=True)
        ring_rd = 0
        sbus_buf = b""
        sbus.active(1)


def sbus_frames():
    """Reports the complete frames in sbus_buf: 0x0F, 22 bytes of channels
    (16 x 11 bits, LSB first), the flags, 0x00 (or an S.Bus2 slot byte)"""
    global sbus_buf
    b = sbus_buf
    skipped = 0
    while len(b) >= 25:
        if b[0] != 0x0F or b[24] & 0x0F not in (0x00, 0x04, 0x08, 0x0C):
            b = b[1:]
            skipped += 1
            continue
        if skipped:
            print("x", skipped)
            skipped = 0
        v = int.from_bytes(b[1:23], "little")
        ch = [(v >> (11 * i)) & 0x7FF for i in range(16)]
        if mon:
            print("s", b[23], " ".join(str(c) for c in ch))
        b = b[25:]
    if skipped:
        print("x", skipped)
    sbus_buf = b


def command(s):
    global pwr, mon
    a = s.split()
    if len(a) == 2 and a[0] == "pwr" and a[1] in ("0", "1"):
        if pwr is None:
            pwr = Pin(PWR, Pin.OUT)
        pwr.value(int(a[1]))
        print("ok pwr=" + a[1])
    elif len(a) == 2 and a[0] == "mon" and a[1] in ("0", "1"):
        mon = a[1] == "1"
        print("ok mon=" + a[1])
    elif len(a) in (2, 3) and a[0] == "in2" and a[1] in ("off", "servo", "sbus"):
        pin = int(a[2]) if len(a) == 3 and a[2].isdigit() else SIG2
        in2(a[1], pin)
        print("ok in2=" + a[1] + " GP" + str(pin))
    elif a:
        print("err " + s)


while True:
    if sbus:
        wr = dma.write - ring_addr
        if wr != ring_rd:
            if wr > ring_rd:
                sbus_buf += ring[ring_rd:wr]
            else:
                sbus_buf += ring[ring_rd:]
                sbus_buf += ring[:wr]
            ring_rd = wr
            sbus_frames()
            continue
    n = sm.rx_fifo()
    if n >= 2:
        if n >= 8:
            print("o", n)
        high = sm.get() * 2 + 5
        low = sm.get() * 2 + 5
        if mon:
            print("p", high, high + low)
    elif sm2 and sm2.rx_fifo() >= 2:
        high = sm2.get() * 2 + 5
        low = sm2.get() * 2 + 5
        if mon:
            print("q", high, high + low)
    elif poll.poll(0):
        c = sys.stdin.read(1)
        if c in "\r\n":
            command(line.strip())
            line = ""
        else:
            line += c
