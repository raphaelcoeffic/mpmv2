# Radio interface

The contract between the radio driver (`src/hal/radio.h`: the CC2652 RF core
today, the SX1281 and a CC27xx later) and what runs on it: the framing layer
(`frame.c`), the virtual chips that will stand in for the CC2500, NRF24L01,
A7105 and CYRF6936, and the MPM scheduler. It is set from what the MPM
protocols need (survey of the MPM source, October 2026) and checked against
what the CC2652 does (`timing`, below).

## What the MPM protocols need

From the protocol and chip driver code of the MPM (`Multiprotocol/`, branch
`mpm-bench`):

**Scheduler.**
- Each protocol callback returns the delay to the next one, in us.
- The deadline is the previous deadline plus that delay, so there is no
  drift.
- Everything RF runs inside the callbacks: register writes, strobes, packet
  build, CRC, telemetry parsing. They use short busy-waits for "TX done", up
  to 700 us.
- Serial input and telemetry output run between callbacks, only when 900 us
  are left (`Multiprotocol.ino:751`).
- The timer is 16 bits at 2 MHz: delays above 32.7 ms wrap (FrSky V bind asks
  for 53.46 ms and gets 20.7 ms).
- Start jitter is the interrupt time plus any overrun of the serial work.
  Protocols carry per-MCU constants for it (AFHDS2A, DSM).

**Frames and turnarounds** (TX protocols):

| Protocol | Frame | Tightest constraint |
|---|---|---|
| FrSky D | 9 ms slots, telemetry every 36 ms | loose: the CC2500 calibrates at each STX (about 0.8 ms) |
| FrSky X / X2 | 9 ms: TX at 0, RX at 5.2 ms, read at 8.6 ms | telemetry ends close to the 8.6 ms read; hop with cached calibration |
| FrSky X EU (LBT) | 9 ms: RX on the next channel, RSSI at 0.4 ms, then TX | instant RSSI valid within 400 us of RX start; reduced power for one packet |
| AFHDS2A | 3.85 ms: TX, RX about 200 us after TX end | TX to RX about 200 us; hop every packet |
| DSM2 / DSMX | 11 or 22 ms: CH1, CH2 4 ms later, then RX | per packet: new SOP code, data code, CRC seed and channel |
| Bayang and most NRF | 2 to 5 ms | TX done within about 1 ms |
| CFlie, Propel, YD717 | ESB with acks | ack within 130 us, retransmissions |
| CABELL | TX then RX after airtime + 140 us | TX to RX 0 to 150 us |

**RX protocols** (FrSky, AFHDS2A, DSM, Bayang):
- Each one polls at a fixed period (0.25 to 1 ms) and hops on a good packet.
- After N empty polls it hops blindly.
- None of them uses packet timestamps: the hop and the RX restart must just
  be done well before the next packet.

**Chip features used:**

| Feature | Protocols |
|---|---|
| Per-packet RSSI, and LQI | FrSky D/X telemetry, FrSky RX, AFHDS2A, Hubsan, DSM |
| Instant RSSI or carrier sense | FrSky X EU LBT, HOTT CCA, scanners, DSM2 channel pick |
| Variable length RX (length byte, NRF dynamic payload) | FrSky, CC2500 protocols, CFlie |
| Per-packet changes: sync word, CRC seed, TX power, channel | DSM, LBT, everything that hops |
| Fine frequency offset (CC2500 FSCTRL0, A7105 base) | FrSky TX and RX auto-tune, A7105 |
| ESB auto-ack, retransmissions, ack payloads | CFlie, Propel, YD717 |
| PA/LNA switch | all |

## What the CC2652 does

`timing.py` runs the bench `timing` command for each PHY. It times the radio
calls with the 48 MHz CPU timer. For the RF core's own timing it uses its PA
and LNA enable outputs (`RFC_GPO1`, `RFC_GPO0`), whose edges are
timestamped in the GPIO edge interrupt. The LNA enable is on a LaunchPad LED
pin; the MPM v2 has no free pin for it, so RX timing comes from the
LaunchPad. October 2026, NRF 1 Mbps, average (maximum) in us:

| What | LaunchPad | MPM v2 |
|---|---|---|
| `radio_init()`: PHY setup | 531 (534) | 540 (546) |
| `radio_set_frequency()`: hop or same channel | 136 (137) | 139 (143) |
| `radio_tx_start()` returns | 11 (12) | 12 (12) |
| TX: call to PA on | 124 (124) | 124 (125) |
| TX: PA on (airtime 216) | 247 | 247 |
| TX: PA off to `radio_tx_wait()` returning | 20 | 20 |
| Hop + TX: call to PA on | 259 (260) | 263 (266) |
| RX running, then TX: call to PA on | 156 (156) | 156 (157) |
| `radio_tx_start_at()`: PA on after the time given | 102 (103) | 102 (103) |
| `radio_rx_start()` returns | 26 (26) | 26 (26) |
| RX: call to LNA on | 67 (67) | |
| `radio_rx_stop()` returns | 32 (32) | 32 (34) |
| RX hop: call to LNA on | 233 (234) | |
| TX end to RX on: `radio_tx_wait()` + `radio_rx_start()` | 88 (88) | |
| TX end to RX on: `radio_send_rx()` (chained) | 79 (79) | |
| Instant RSSI: `radio_rx_start()` to valid | 99 (101) | 100 (101) |
| One `radio_rssi()` | 10 | 10 |

- **The same on every proprietary PHY** (NRF 250 kbps, FrSky D and X,
  AFHDS2A, DSM): within 1 us, except the RSSI, valid after 150 us at FrSky
  D and X.
- **The Bluetooth LE 2M PHY** (NRF 2 Mbps): setup 696 us, hop 166 us, RX on
  6 us later.
- **Spread:** all within 1 to 3 us from run to run.
- **PA on beyond the airtime:** about 6 bit times + 20 us, the ramps on both
  sides of the packet (31 us at 1 Mbps, 227 us at FrSky D's 31 kbps).
- **TX start is fixed:** with the synthesizer already in TX mode (`CMD_FS`
  `bTxMode`), the TX command still takes 124 us. It relocks the synthesizer
  itself, it seems.

**RX timestamps.**
- The RF core appends its timer (RAT, 4 MHz, same clock as the CPU) to each
  packet. `radio.c` converts it to `micros()`.
- The time it gives is the sync word found minus the configured preamble and
  sync word lengths (TI's nominal packet start), measured:
  - shifts of exactly 8 bit times per configured preamble byte;
  - the same for each sync word byte.
- `radio.c` adds them back: `timestamp_us` is when the sync word was found.
  The demodulator delay in it is not measured: the same for every packet of
  a PHY.
- The packet reaches the CPU (RX entry interrupt) 20 bit times + 40 us after
  its last bit: 60 us at 1 Mbps, 120 us at 250 kbps.
- At 70 kbps (FrSky X) that would be about 330 us (extrapolated). The
  CC2500 has the first bytes in its FIFO while the packet arrives, which
  FrSky X's 8.6 ms read counts on.

**Against the needs:**

| Need | CC2652 |
|---|---|
| TX start within about 150 us, deterministic | 124 us, +-1 us; or at a set time (102 us after it) |
| TX end to RX within 130 us (ESB) / 200 us (AFHDS2A) | 79 us chained, 88 us unchained |
| Hop within about 100 us (CC2500 with cached calibration) | 136 us, 260 us to TX: fine when TX is scheduled ahead |
| Instant RSSI within 400 us of RX (LBT) | 100 to 150 us |
| PHY change (virtual chip reconfigured) | 530 us: only when the PHY itself changes |
| Packet available soon after its end | 20 bit times + 40 us after its end, its length read from it (at the earliest 100 us after the sync word); no partial packet |

## The interface

`src/hal/radio.h`. Times are `micros()`. One operation runs at a time: each call
first waits for a TX started before, and stops RX.

**Setup.**
- `radio_init(phy)`: power up and apply a `radio_phy_t`. It takes 530 us, so
  call it only when the PHY changes; sync words, frequency and power don't
  need it.
- `radio_off()`.

- `radio_set_tx_power(word)`: `CMD_SET_TX_POWER`, from the next packet on,
  without `radio_init()` (LBT's reduced power for one packet). The PA sees it
  on the air: -34 dBm received at the default word, -59 dBm at `0x04c6`,
  packet by packet.

**Channel.**
- `radio_set_frequency(khz)`: blocks for the 136 us of `CMD_FS`.
- The frequency trim is the caller's: kHz added to the frequency.

**Sync words.**
- `radio_set_sync_word(tx, rx)` and `radio_set_rx_sync_alt()`.
- Used by the next TX and RX commands; nothing is reprogrammed.

**TX.**
- `radio_tx_start(data, len)` returns in 11 us. The PA comes on 124 us
  after the call.
- `radio_tx_start_at(data, len, at_us)`: the RF core starts it at `at_us`,
  and the PA comes on 102 us later. This removes the callback jitter: a
  protocol can schedule its packet one callback ahead.
- `radio_tx_done()`: has the packet gone? `radio_tx_wait()`: wait for it.
  `data` must stay valid until then.
- `radio_send()`: start and wait.

**RX.**
- `radio_rx_start(len)`: continuous, fixed length.
- `radio_rx_start_var(hdr_len, max_len, len_fn, ctx)`: continuous, variable
  length. After `hdr_len` bytes, `len_fn()` (in the RF core interrupt) gives
  the packet length from them; `frame_rx_hdr_len()` and `frame_rx_var_len()`
  do it for the NRF dynamic payload length, XN297 enhanced mode, HS6200 and
  the CC2500 length byte (whitening, FEC).
  - The RF core takes the length about 100 us after the sync word at the
    earliest: a shorter packet ends there.
  - One packet per RF core command: RX starts again from the interrupt at the
    end of each packet, about 70 us to RX on.
  - The interrupt takes about 12 us per packet.
  - On the Bluetooth LE 2M PHY: the maximum length (short packets end with an
    error there).
  - Measured: NRF dynamic payload 1 to 32 bytes at 250 kbps and 1 Mbps, all
    received; CC2500 protocols as with fixed length.
- `radio_receive(pkt)`: polls. Each packet has its RSSI, the modem's
  frequency estimate and `timestamp_us` (sync word found).
- `radio_rx_stop()`, `radio_rx_active()`, `radio_rx_stats()`.

**TX then RX.**
- `radio_send_rx()`: chained, for ESB acks, with a window from RX start.

**Instant RSSI.**
- `radio_rssi()` while RX runs: `RADIO_RSSI_NONE` until valid.

**Still to add,** in the order the virtual chips will need them:

1. **RX with an end:** an RX window that ends by itself (`endTrigger`), as
   `radio_send_rx()` has; for the telemetry slots.
2. **A non-blocking channel change:** `CMD_FS` chained in front of the next TX
   or RX command, so that the CPU doesn't wait 136 us per hop.
3. **LQI:** the RF core has none. The virtual CC2500 can derive it from the
   RSSI and the frequency estimate, or report a constant.
4. **Partial packets:** if a protocol needs the first bytes before the end
   (FrSky X telemetry at 8.6 ms): the variable length RX entries fill as the
   data arrives, `radio_receive()` could hand them out early.

**The scheduler** (`src/hal/timer.h`, for the MPM core port):
`timer_set_alarm(at_ticks, callback)`, an alarm at an absolute time of the
48 MHz free-running timer (GPT3 match interrupt), up to 44 s ahead. A
periodic task adds its period to the last deadline (no drift), as the MPM
protocol callbacks need. Bench `sched`, October 2026, LaunchPad (the MPM v2
the same):

| Period | Callback start after its deadline, avg (max) |
|---|---|
| 100 us to 9 ms | 1.2 us (1.5) |
| 32.768 ms, 53.46 ms (FrSky V bind, beyond the MPM's 16 bit timer), 100 ms, 3 s | 1.2 us (1.3) |
| 1 ms, while receiving and printing 1000 packets/s (variable length RX) | 1.2 us (1.7) |

All interrupts have the same priority: another one running at the deadline
delays the alarm by its length (the RX interrupt: about 12 us). The MPM port
can raise the alarm's priority, and start TX on the RF core's clock
(`radio_tx_start_at()`) one callback ahead.

The SX1281 driver (`src/drivers/sx1281.h`, bench `chip sx1281`) has functions of
the same shape (`sx1281_send()`, `sx1281_rx_start()`, ...), without the ones
added here (`radio_tx_start()` and `radio_tx_start_at()`, `radio_rssi()`): a
common interface over both chips is for when the chip is chosen.
