#!/usr/bin/env python3
"""sbus2mon host side (main.c): log the Pico's groups, decode them.

  sbus2.py log OUT SECS [SENSOR...]  @g lines to OUT for SECS; the Pico's
                                 slots set to the SENSORs first (none: all off)
  sbus2.py dec LOG [-v|-s]       frames and slots: a summary; -v every frame
                                 and slot; -s "time ch1..16 flags XX" on change
                                 (salrx.py -v's format, for dec14.py)

S.BUS frame: 0x0F, 16 x 11-bit channels LSB first, flags (bit 0 ch17, bit 1
ch18, bit 2 frame lost, bit 3 failsafe), end byte (S.BUS2: 0x04 / 0x14 /
0x24 / 0x34 = slots 0..7 / 8..15 / 16..23 / 24..31 follow). A slot: 3 bytes,
its ID byte then 2 data bytes; slot i's offset from the frame start is
printed (us), so the slot timing can be checked.

SENSOR = type:slot=values (the data bytes as BrushlessPower's SBUS2-Telemetry
library sends them; slot 0 is the receiver's own):
  temp:N=C            SBS-01T, 1 slot: (C + 100) | 0x8000, low byte first
  temp125:N=C         SBS-01TE, 1 slot: C | 0x4000
  rpm:N=RPM           SBS-01RM, 1 slot: RPM / 6, low byte first
  volt:N=V1,V2        SBS-01V, 2 slots: V1 x 10 | 0x8000, V2 x 10
  curr:N=A,V,MAH      SBS-01C, 3 slots: A x 100 (bit 14 set), V x 100, mAh
  raw:N=HHHH          the 2 data bytes
"""
import sys, time


def find_port():
    from serial.tools import list_ports
    for p in list_ports.comports():
        if p.vid == 0x2E8A: return p.device
    sys.exit("no Pico found")


def sensor_slots(spec):
    """type:slot=values -> [(slot, data word)], data word = byte 1 << 8 | byte 2"""
    kind, rest = spec.split(":", 1)
    n, vals = rest.split("=", 1)
    n, v = int(n), [float(x) for x in vals.split(",")] if kind != "raw" else [int(vals, 16)]
    be = lambda x: x & 0xFFFF
    le = lambda x: (x & 0xFF) << 8 | (x >> 8 & 0xFF)
    if kind == "temp": return [(n, le(((int(v[0]) | 0x8000) + 100) & 0xFFFF))]
    if kind == "temp125": return [(n, be(int(v[0]) | 0x4000))]
    if kind == "rpm": return [(n, le(min(int(v[0]) // 6, 0xFFFF)))]
    if kind == "volt": return [(n, be(min(int(round(v[0] * 10)) | 0x8000, 0x9FFF))),
                               (n + 1, be(min(int(round(v[1] * 10)), 0x1FFF)))]
    if kind == "curr":
        a = min(int(round(v[0] * 100)), 0x3FFF)
        return [(n, (((a >> 8) | 0x40) & 0x7F) << 8 | (a & 0xFF)),
                (n + 1, be(int(round(v[1] * 100)))), (n + 2, be(int(v[2])))]
    if kind == "raw": return [(n, be(v[0]))]
    sys.exit("unknown sensor " + kind)


def log(out, secs, sensors=()):
    import serial
    s = serial.Serial(find_port(), 115200, timeout=0.2)
    cmds = ["clear"] + ["slot %d %04x" % sl for spec in sensors for sl in sensor_slots(spec)]
    for c in cmds: s.write((c + "\n").encode()); time.sleep(0.05)
    print("\n".join(cmds))
    s.reset_input_buffer()
    end, n = time.monotonic() + secs, 0
    with open(out, "w") as f:
        while time.monotonic() < end:
            for l in s.read(65536).decode(errors="replace").splitlines(True):
                f.write(l); n += l.startswith("@g")
    print("%s: %d groups" % (out, n))


def groups(path):
    for l in open(path):
        p = l.split()
        if len(p) >= 3 and p[0] == "@g":
            yield int(p[1]), bytes.fromhex(p[2]), len(p) > 3


def channels(b):
    w = int.from_bytes(b[1:23], "little")
    return [w >> (11 * i) & 0x7FF for i in range(16)]


def dec(path, mode):
    frames = slots = errs = 0
    t0 = tf = None; prev = None; ends = {}; offs = {}
    last_t = None
    for t, b, err in groups(path):
        # the Pico's 32-bit us timer wraps every 71.6 min
        if last_t is not None and t < last_t - (1 << 31): t += 1 << 32
        last_t = t
        if t0 is None: t0 = t
        errs += err
        if len(b) == 25 and b[0] == 0x0F:
            frames += 1; tf = t; end = b[24]; ends[end] = ends.get(end, 0) + 1
            ch, fl = channels(b), b[23]
            if mode == "-v": print("%.6f frame %s flags %02x end %02x%s" % ((t - t0) / 1e6, " ".join(map(str, ch)), fl, end, " ERR" if err else ""))
            if mode == "-s" and (ch, fl) != prev: print("%.4f" % ((t - t0) / 1e6), " ".join(map(str, ch)), "flags %02x" % fl)
            prev = (ch, fl)
        else:
            slots += 1
            off = t - tf if tf is not None else -1
            key = (b[0], round(off / 10) * 10 if off >= 0 else -1)
            offs[key] = offs.get(key, 0) + 1
            if mode == "-v": print("%.6f slot +%d us %s%s" % ((t - t0) / 1e6, off, b.hex(" "), " ERR" if err else ""))
    if mode in ("-v", "-s"): return
    span = (last_t - t0) / 1e6 if last_t is not None else 0
    print("%s: %.1f s, %d frames, %d other groups, %d with errors" % (path, span, frames, slots, errs))
    print("end bytes:", " ".join("%02x:%d" % kv for kv in sorted(ends.items())))
    print("slot ID byte / offset from frame start (10 us bins):")
    for (i, o), n in sorted(offs.items(), key=lambda kv: (kv[0][1], kv[0][0])):
        print("  %02x +%d us: %d" % (i, o, n))


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[:1] == ["log"]: log(a[1], float(a[2]), a[3:])
    elif a[:1] == ["dec"]: dec(a[1], a[2] if len(a) > 2 else "")
    else: sys.exit(__doc__)
