# CC2652 RF core notes

What was found about the CC13x2/CC26x2 RF core beyond TI's documentation,
while getting 2 Mbps RX to work (October 2026). Measured on the LaunchPad
(CC2652R1); the MPM v2's CC2652RB behaves the same where tried.

## Credits

The starting point is the 38C3 talk "Beyond BLE: Cracking open the black-box
of RF microcontrollers" by Adam Batori and Robert Pafford (December 2024):
[talk](https://events.ccc.de/congress/2024/hub/en/event/beyond-ble-cracking-open-the-black-box-of-rf-microcontrollers/),
[tools](https://github.com/rjp5th/beyond-ble-tools). It showed that the RF
core (a Cortex-M0 "CPE", plus the "MCE" modem and "RFE" front-end
sequencers) can be explored and patched, and its modem register list
(`ghidra_topsm/ghidra_scripts/modem_reg_create.py`) gives the names used
below. Thanks to them for the ideas and the inspiration.

The other source is TI's own SDKs: the radio configuration database of the
CC13xx/CC26xx SDK (PHY settings with their overrides) and the CC27xx files of
the SimpleLink Low Power F3 SDK (register headers with field names, PHY
register values), whose modem is the same IP.

## Reading the RF core

The debugger reads the RF core registers while it runs (OpenOCD, XDS110 of
the LaunchPad):

```bash
openocd -f board/ti_cc26x2_launchpad.cfg -c init -c "mdw 0x40045000 256" -c exit
```

| Range | Content |
|---|---|
| `0x21000000` | CPE RAM, the parameter table at `0x21000028` (below) |
| `0x21008000`, `0x2100C000` | MCE and RFE RAM |
| `0x40041000` | Doorbell; `0x40041100..` packet hardware (below) |
| `0x40042000` | CRC unit: TI's wBMS setting writes its polynomial at `0x2004` |
| `0x40044000` | Synthesizer, the same words repeated over the block |
| `0x40045000` | Modem (MCE side), mirrored every 1 KB |
| `0x40046000` | RFE |
| `0x40047000` | **Don't read it**: the bus hangs, only a hardware reset (`reset_config srst_only`) brings the chip back |

The modem block hangs the bus the same way, from the CPU or the debugger:

- `0x5058..0x509F` (FIFO / mailbox words) during RX.
- Any modem register while its clock is off. The CPE turns it on and off
  itself (`RFC_PWR` `PWMCLKEN`, MDM bit 3): on while an RX or TX command runs,
  off after its end, e.g. an RX ended by a full queue. `radio.c` reads the
  modem only from the RX entry interrupt, during a repeated RX with a free
  entry left ([Frequency estimate](#frequency-estimate)).

The bench `ovr` command appends raw override words to the setup (both PHYs):
any register or parameter can be tried without rebuilding.

## Modem registers

Word `i` of the modem block (`0x40045000 + 4 * i`, override offset `0x5000 +
4 * i`) is entry `i` of the Beyond BLE register list. The field layouts are
those of the CC27xx `LRFDMDM` registers (`hw_lrfdmdm.h` of the LPF3 SDK, at
other offsets). Checked: TI's Bluetooth values decode the same on both
(matched filter, symbol tracking).

| Offset | Register | Use |
|---|---|---|
| `0x50D4` | `DEMMISC2` | MAFCGAIN (13:12), STIMBYPASS (11), STIMESTONLY (10), STIMTEAPERIOD (9:7), STIMTEAGAIN (6:4), PDIF bits (3:0). TI's "symbol tracking" override writes 0x00F9 |
| `0x5100..0x5108` | `DEMMAFI0..2` | Matched filter: C1C7 (15:8 of MAFI0), C3C5 / C2C6 (MAFI1), C4 (8:0 of MAFI2) |
| `0x5110` | `DEMC1BE0` | Sync correlator: MASKB, MASKA, CASCCONF (5:4: single, serial, parallel), COPYCONF (3:0) |
| `0x5114` | `DEMC1BE1` | Sync thresholds B (15:8) and A (7:0): the bench `syncthr` |
| `0x5118` | `DEMC1BE2` | PEAKCONF (9:8: threshold, best, best of A and B), THRESHOLDC (7:0) |
| `0x5164..0x5170` | `DEMC1BEREF0..3` | Correlator references A and B (the sync words) |
| `0x50E0`, `0x50E4` | `DEMDSBU`, `DEMDSBU2` | DSBUDELAY, DSBUAVGLENGTH (0x83, 0x80) |
| `0x50C0`, `0x50C4` | `MDMBAUD`, `MDMBAUDPRE` | Symbol rate (0x8000 / 12 on the Bluetooth 2M PHY) |
| `0x51C0` | `DEMMAFC0` | COMPVAL (7:0): frequency offset of the last packet, signed ([below](#frequency-estimate)) |
| `0x52C4` | `DEMDSBU1` | AVGVAL (15:0): follows the offset too, with an offset of its own per PHY |

For NRF 2 Mbps on the Bluetooth LE 2M PHY, two values of TI's CC27xx NESB PHY
(`nesb_1m_2m_cc27xx.json`) are used: MAFCGAIN 1 and the matched filter for
modulation index 0.32 ([results](interop-results.md#2-mbps-rx-the-bluetooth-le-2m-phy)).

## Frequency estimate

`DEMMAFC0` (COMPVAL, "matched filter AFC compensation" it seems) holds the
frequency offset of the last packet received, from its start: it changes at
the next sync, and between packets it reads the same. It is there on every
proprietary PHY, without TI's MAFCGAIN override. Measured against the trim,
which moves the CC2652 by a known amount:

- Linear over +-100 kHz (narrow PHYs: as far as packets get through), noise
  of about 1 count from packet to packet.
- Positive when the sender is above the receive frequency.
- About 61 counts per deviation (52 to 65 on most setups: NRF 1 Mbps,
  FrSky X 51, Redpine slow 34): 0.5 kHz per count at FrSky D (31.75 kHz
  deviation), 4 kHz at the A7105 PHYs (250 kHz). On the Bluetooth LE 2M PHY
  (500 kHz deviation): as 326 kHz, about 5 kHz per count.
- Its zero moves a little with the PHY: against the same chip, NRF 1 Mbps and
  250 kbps 13 kHz apart, A7105 500 and 100 kbps 22 kHz, DSM and Devo 38 kHz
  (same CYRF6936, other channel and setup).

`DEMDSBU1` (AVGVAL) moves the same way, at the same scale, but its zero
depends more on the PHY (NRF 1 Mbps: 74 kHz from `DEMMAFC0`'s, Devo: 0).

`radio.c` reads `DEMMAFC0` in the RX entry interrupt (`IRQ_RX_ENTRY_DONE`):
later, the next packet or a new RX command could have replaced it (late
reads during DSM reception gave 36 on many packets), and the modem clock
could be off. `radio_packet_t`
gives it in kHz (`freq_offset`, counts x deviation / 61), the bench prints it
as `@rx ... foff=` and its `autotrim` follows it
([results](interop-results.md#automatic-frequency-trim)).

## Sync correlator

The proprietary RX runs correlator A alone (`DEMC1BE0` 0x000F, single), on
its best peak (`DEMC1BE2` 0x017F), threshold `syncthr` (`0x5114`). TI's
CC27xx generic GFSK and NESB PHYs run A and B in parallel (two sync words),
best of the two, threshold 39 (0x27); 0x7F disables a correlator.

Setting them:

- The modem firmware writes `DEMC1BE0`, `DEMC1BE2`, `DEMMISC2` and
  `DEMDSBU` at RX start: setup overrides of those don't stick (the
  thresholds `0x5114` and `DEMDSBU2` do).
- `poke` during RX does change `DEMC1BE2` (peak mode). Writing `DEMMISC2` or
  `DEMDSBU` / `DEMDSBU2` that way stopped all reception, even of strong sync
  words, until the next setup.
- `DEMC1BE3` (CORRVALUEA in the CC27xx headers) reads 0.

What it detects: 32 bit test words inside a 26 byte NRF payload behind
address `e7e7e7e7e7`, raw framing with the word as sync word, guard bytes
around it so that no shift of 1 to 16 bits comes close (LaunchPad, 1 Mbps,
50 to 100 packets per point, packets found at any bit offset):

| Word | Synced at threshold 0x20 / 0x28 / 0x32 / 0x3C / 0x46 |
|---|---|
| `a5c3e19b`, `ff00ff00`, `c3c3c3c3` | 88-100 / 94-100 / 100 / 22-94 / 0% |
| `550f0f0f`, `55656565` | 100 / 100 / 94-96 / 2-56 / 0% |
| `0f0f0f0f` | 16-25 / 100 / 94-100 / 100 / 0% |
| `33333333` | 0 / 0 / 100 / 58 / 0% |
| `55555555` (0101) | 22 / 24 / 56-64 / 0 / 0% |
| `0000ffff` | 8-24 / 6 / 46-54 / 0 / 0% |
| `55000000` | 4 / 80 / 0-13 / 0 / 0% |
| `00000000`, `ffffffff` | 0 to 8% at any threshold, trim -200 to +200 |

- The packets that sync, sync at the right bit (no early or late ones in
  this layout): the losses are syncs not found.
- Words with long runs or without transitions never reach the threshold,
  even received without a bit error: the correlation seems to be taken
  after the local average is removed, so a run of ones or zeros counts for
  little, and so does 0101 (the Gaussian filter flattens it). Balanced
  words with runs of 2 to 8 bits reach about 0x3C to 0x46.
- Words far from balanced also depend on the frequency offset:
  `55000000` 0 to 62% from trim +200 to -200.
- Lower thresholds sync earlier on near matches (the periodic words at
  0x20, the packet header), which loses the packet.

So the weak NRF addresses are weak for the correlator itself, not only for
the early syncs `frame.c` avoids: `5555555555` (the preamble run) and
`0000000000`, `000000` have no strong 32 bit window at all. A 3 byte
`000000` (ESky bind) is never received; `5555555555` about 25% synced. No
correlator setting found brings them back.

## Timing

Measured with the bench `timing` command ([radio-interface.md](radio-interface.md)):

- **PA and LNA enable:**
  - `RFC_GPO1` is high from the PA ramp-up to the end of the ramp-down: the
    airtime + about 6 bit times + 20 us.
  - `RFC_GPO0` is high while RX runs.
  - A pin routed to them by the IOC (`IOC_PORT_RFC_GPO0/1`) keeps its input
    buffer and edge interrupt when enabled, so the CPU can timestamp the
    edges.
- **TX start:**
  - `CMD_PROP_TX` turns the PA on 124 us after the doorbell, and 102 us
    after its start time with `TRIG_ABSTIME`.
  - It is the same with the synthesizer already tuned in TX mode (`CMD_FS`
    `synthConf.bTxMode` 1): the TX command relocks it itself, it seems.
  - `CMD_FS` takes 136 us, the same for a 20 MHz hop or the same frequency.
- **RX timestamp** (`rxConf.bAppendTimestamp`, 4 bytes after the RSSI byte):
  - The RAT time of the sync word found, minus the configured preamble
    (`preamConf.nPreamBytes`) and sync word (`formatConf.nSwBits`) lengths.
  - Measured: 8 bit times less per preamble or sync word byte, and the same
    against the packet end at any length.
  - The RX entry interrupt comes 20 bit times + 40 us after the last bit.
  - On the Bluetooth LE 2M setup the offset fits its own 2 byte preamble and
    32 bit access address (24 us).
- **RAT and CPU:**
  - The RAT (`RFC_RAT:RATCNT`, `0x40043004`, 4 MHz) runs from the same HF
    clock as the CPU timers.
  - Read with `micros()` with interrupts masked, one converts to the other
    exactly.
- **Instant RSSI** (`CMD_GET_RSSI`, direct command, the value in `CMDSTA`
  bits 23:16):
  - -128 until valid: 99 us after the RX call at 250 kbps and above, 150 us
    at FrSky D and X.
  - 10 us per call.

## Variable length RX

`radio_rx_start_var()`: `CMD_PROP_RX_ADV` with `maxPktLen` 0 (unlimited),
one partial read entry (`DATA_ENTRY_TYPE_PARTIAL`, `lenSz` 0) per command.

- **Data as it comes:** the RF core writes the packet into the entry 2 bytes
  at a time (`nextIndex`), with `IRQ_RX_DATA_WRITTEN` each time and
  `IRQ_RX_N_DATA_WRITTEN` every `config.irqIntv` bytes.
- **`CMD_PROP_SET_LEN`** ends the packet at `rxLen` data bytes; RSSI and
  timestamp follow, status `PROP_DONE_OK`. Without it the data fills the
  entry: `PROP_ERROR_RXFULL`.
  - Send it as the command structure: `CMDR_DIR_CMD_2BYTE` drops the low 2
    bits of the length.
  - The RF core takes it about 100 us after the sync word at the earliest
    (1 Mbps: the doorbell acknowledged 68 us after a command sent 2 bytes in,
    29 us after one sent 8 bytes in; 250 kbps: 19 us after 8 bytes). A
    shorter packet ends there, with what came after it.
  - Waiting for the acknowledgment blocks the CPU that long: `radio.c` writes
    the doorbell and goes on (the next doorbell command waits for it).
  - A length below what is received by then: `PROP_DONE_OK` at 1 Mbps for a
    5 byte length 10 bytes in, `PROP_DONE_RXERR` for 2. On the Bluetooth LE
    2M setup, `PROP_DONE_RXERR` for packets up to 19 bytes (acknowledged
    80 to 100 us after the command): `radio.c` receives the maximum length
    there.
- **The entry at the end:** `status` stays active while the command runs
  (`pktStatus.numElements` 1), and the RF core sets it finished at the end of
  the command, before `IRQ_LAST_COMMAND_DONE`: `radio.c` marks the packets
  itself.
- **`CMD_PROP_RESTART_RX`** goes back to sync search but leaves the bytes in
  the entry, ahead of the next packet (which then ends with
  `PROP_ERROR_RXFULL`). `radio.c` ends a packet whose length is not valid
  with `CMD_PROP_SET_LEN` at the bytes received (`PROP_DONE_RXERR`) and
  starts RX again.
- **The output counters** (`rfc_propRxOutput_t`) add up over the commands.

## CPE parameter table

The CPE keeps the packet settings in a table at `0x21000028`, written by
TI's `SW_REG_BYTE_OVERRIDE` (`0x8003 | index << 4 | value << 16`) and
`SW_ARRAY_OVERRIDE` words. Known entries:

| Index | Content | Values |
|---|---|---|
| `0x10` | Packet format | proprietary setup: 0x04 (MSB first, `bMsbFirst`), 0x07 with PN9 whitening; Bluetooth setup: 0x01 (LSB first, Bluetooth whitening). So bit 2 = MSB first, bit 0 = whitening, bit 1 = PN9 rather than the Bluetooth LFSR (guess from the two values) |
| `0x11` | CRC length in bits | 16 (proprietary), 24 (Bluetooth), 32 (TI's wBMS override) |
| `0x14` | CRC init (32 bits) | 0x555555 << 8 (Bluetooth) |

The Bluetooth whitening seen on the proprietary commands is the Bluetooth
LFSR (x^7 + x^4 + 1) seeded 0x65, advertising channel 37, on every channel.
`rf_cc26x2.c` writes 0x04 at index 0x10 (override `0x00048103`) on the Bluetooth
setup: MSB first, no whitening, as on the proprietary PHY.

`0x4004110C` reads 3 with the proprietary PN9 whitening, 2 without it and on
the Bluetooth setup: a PN9 whitener enable, it seems.

## Patches

TI's CPE patches (`rf_patch_cpe_prop`, `rf_patch_cpe_bt5`) copy their image
only once per boot and count on the retained RF core RAM. Loading the other
one after a power cycle of the RF core hung the next setup (status ACTIVE):
`rf_cc26x2.c` clears both patches' state (`cleanPropPatch()`, `cleanBt5Patch()`)
before loading one.
