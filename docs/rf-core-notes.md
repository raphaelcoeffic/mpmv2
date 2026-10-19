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
| `0x5110` | `DEMC1BE0` | Sync correlator: MASKB, MASKA, CASCCONF (5:4), COPYCONF (3:0) |
| `0x5114` | `DEMC1BE1` | Sync thresholds B (15:8) and A (7:0): the bench `syncthr` |
| `0x5118` | `DEMC1BE2` | PEAKCONF (9:8), THRESHOLDC (7:0) |
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
`radio.c` writes 0x04 at index 0x10 (override `0x00048103`) on the Bluetooth
setup: MSB first, no whitening, as on the proprietary PHY.

`0x4004110C` reads 3 with the proprietary PN9 whitening, 2 without it and on
the Bluetooth setup: a PN9 whitener enable, it seems.

## Patches

TI's CPE patches (`rf_patch_cpe_prop`, `rf_patch_cpe_bt5`) copy their image
only once per boot and count on the retained RF core RAM. Loading the other
one after a power cycle of the RF core hung the next setup (status ACTIVE):
`radio.c` clears both patches' state (`cleanPropPatch()`, `cleanBt5Patch()`)
before loading one.
