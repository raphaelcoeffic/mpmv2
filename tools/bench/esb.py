#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""Enhanced ShockBurst: CC2652 PTX ("ack": waits for the ack, retransmits) ->
MPM NRF24L01 PRX (EN_AA), address a1b2c3d4e5, ch 40, 16 byte payload.

  esb.py --rate 1m                       dynamic payload length, acks without payload
  esb.py --rate 250k --dpl 0             static length
  esb.py --rate 250k --ackpl 00a1a2a3    ack payloads (byte 0 counts), 4 bytes
  esb.py --rate 250k --ackpl <32 bytes> --window 1500

The ack window must cover the ack with its payload (the NRF's ARD): 1500 us
for 32 bytes at 250 kbps. 2 Mbps: the NRF receives, the CC2652 gets no ack on
its proprietary PHY (--rate 2m), it does on the Bluetooth LE 2M PHY (--rate 2mble).
"""

import argparse
import sys
import time

import rig

ADDR = "a1b2c3d4e5"
# 2mble: CC2652 on the Bluetooth LE 2M PHY (preset nrf2mble), which receives 2 Mbps
RF_SETUP = {"250k": "0x22", "1m": "0x02", "2m": "0x0a", "2mble": "0x0a"}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rate", default="250k", choices=RF_SETUP)
    ap.add_argument("--dpl", type=int, default=1, choices=(0, 1), help="dynamic payload length (default 1)")
    ap.add_argument("--retries", type=int, default=3)
    ap.add_argument("--window", type=int, default=500, help="ack window in us (default 500)")
    ap.add_argument("--ackpl", help="NRF ack payload (hex, needs --dpl 1), byte 0 counts")
    ap.add_argument("--period", type=int, default=5000)
    ap.add_argument("--count", type=int, default=250)
    a = ap.parse_args()
    if a.ackpl and not a.dpl:
        ap.error("ack payloads need dynamic payload length")

    feature = "0x06" if a.ackpl else "0x05" if a.dpl else "0x01"  # EN_DPL, EN_ACK_PAY, EN_DYN_ACK
    mpm_setup = ["preset nrf", f"w 0x06 {RF_SETUP[a.rate]}", "w 0x00 0x0e", f"w 0x0a {ADDR}", f"w 0x10 {ADDR}",
                 "w 0x11 0x10", "w 0x01 0x01", f"w 0x1d {feature}", f"w 0x1c {'0x01' if a.dpl else '0x00'}", "ch 40"]
    if a.ackpl:
        mpm_setup += [f"ackpl {a.ackpl}", "seq 0"]
    cc_setup = [f"preset nrf{a.rate}", f"nrf {ADDR} 2 4", f"esb 1 {a.dpl}",
                f"ack 1 {a.retries} {a.window} {32 if a.ackpl else 0}", "ch 40", "seq 1"]

    mpm, cc = rig.open_devices()
    with mpm, cc:
        rig.run(mpm, mpm_setup)
        mpm.cmd("rx 0" if a.dpl else "rx 16")
        rig.run(cc, cc_setup)
        rig.drain(cc)
        cc.cmd(f"txp {a.period} {a.count} {rig.pattern(16).hex()}")
        acks, ack_counters, end = 0, set(), time.monotonic() + a.count * a.period / 1e6 + 3
        while time.monotonic() < end:
            try:
                e = cc.events.get(timeout=0.5)
            except Exception:
                continue
            if e.startswith("@ack"):
                acks += 1
                ack_counters.add(e.split()[3][:2])
            elif e.startswith("@ev txdone"):
                break
        time.sleep(0.1)
        seqs = {e.split()[3][2:4] for e in rig.events(mpm) if rig.flag(e, "crc") != "bad"}
        cc_stats, mpm_stats = cc.cmd("stop"), mpm.cmd("stop")
    print(f"{a.rate} dpl={a.dpl} retries={a.retries} window={a.window}"
          + (f" ackpl={len(a.ackpl) // 2}B" if a.ackpl else ""))
    print(f"  CC2652 PTX: {cc_stats}" + (f" | @ack={acks} distinct ack counters={len(ack_counters)}" if a.ackpl else ""))
    print(f"  NRF PRX: distinct packets {len(seqs)}/{min(a.count, 256)} | {mpm_stats}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
