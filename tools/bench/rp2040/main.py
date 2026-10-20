# Servo output monitor, MicroPython on an RP2040-Zero (tools/bench/servo_mon.py):
# a receiver's servo output on GP12 (through a series resistor), its supply
# switched by GP11 (high-side switch).
#
# Commands (lines on the USB serial):
#   pwr 0|1   receiver supply (GP11 level), "ok pwr=N"
#   mon 0|1   pulse report on/off, "ok mon=N"
# Reports: "p <high> <period>" per pulse in 0.1 us units, "o <n>" when n pulses
# were lost (FIFO full).

import rp2
import select
import sys
from machine import Pin

SIG, PWR = 12, 11


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


sm = rp2.StateMachine(0, pulse, freq=10_000_000, in_base=Pin(SIG, Pin.IN), jmp_pin=Pin(SIG))
sm.active(1)
pwr = None
mon = True
poll = select.poll()
poll.register(sys.stdin, select.POLLIN)
line = ""


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
    elif a:
        print("err " + s)


while True:
    n = sm.rx_fifo()
    if n >= 2:
        if n >= 8:
            print("o", n)
        high = sm.get() * 2 + 5
        low = sm.get() * 2 + 5
        if mon:
            print("p", high, high + low)
    elif poll.poll(0):
        c = sys.stdin.read(1)
        if c in "\r\n":
            command(line.strip())
            line = ""
        else:
            line += c
