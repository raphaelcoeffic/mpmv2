/*
 * S.Port sensor emulator for a Raspberry Pi Pico: answers a receiver's
 * S.Port polls as a FrSky vario (vertical speed 0x0110 in cm/s, altitude
 * 0x0100 in cm, in turn), on one GPIO: two PIO state machines (sport.pio)
 * share the pin, the TX one drives it only while sending. The line is
 * inverted in the GPIO block, so no inverter is needed.
 *
 * Wiring: SPORT_PIN -> the receiver's S.Port pin, GND; POWER_PIN -> a
 * receiver power switch (high = on), if any.
 *
 * USB serial (any baud rate), one command per line:
 *   id <0..27>     physical ID to answer (default 0, the vario's)
 *   quiet <0|1>    1: don't answer (sensor unplugged)
 *   hold <cm/s>    vertical speed fixed at that value (altitude follows)
 *   ramp           back to the triangle
 *   power <0|1>    receiver power (POWER_PIN)
 *   bootsel        reboot to the USB boot loader
 * Once a second: P polls=<all polls> mine=<to our ID> replies=
 *   others=<to other IDs> ids=<bit mask of the polled IDs> vspeed= alt=
 * vspeed is a +-1 m/s triangle (0.1 m/s a second, 40 s period), alt follows.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "sport.pio.h"

#define BAUDRATE    57600
#define START_BYTE  0x7E
#define STUFF_BYTE  0x7D
#define STUFF_XOR   0x20
#define DATA_FRAME  0x10
#define REPLY_US    200     /* after the poll's ID byte */

#define VARIO_ID    0x0110
#define ALT_ID      0x0100

static const uint8_t sport_ids[28] = {
    0x00, 0xA1, 0x22, 0x83, 0xE4, 0x45, 0xC6, 0x67, 0x48, 0xE9, 0x6A, 0xCB, 0xAC, 0x0D,
    0x8E, 0x2F, 0xD0, 0x71, 0xF2, 0x53, 0x34, 0x95, 0x16, 0xB7, 0x98, 0x39, 0xBA, 0x1B,
};

static PIO      pio = pio0;
static unsigned sm_rx, sm_tx;

static void line_init(void)
{
    float    div = (float)clock_get_hz(clk_sys) / (8 * BAUDRATE);
    unsigned off_rx = pio_add_program(pio, &sport_rx_program);
    unsigned off_tx = pio_add_program(pio, &sport_tx_program);

    sm_rx = pio_claim_unused_sm(pio, true);
    sm_tx = pio_claim_unused_sm(pio, true);

    pio_gpio_init(pio, SPORT_PIN);
    /* pio_gpio_init() clears the overrides: set them afterwards */
    gpio_set_inover(SPORT_PIN, GPIO_OVERRIDE_INVERT);
    gpio_set_outover(SPORT_PIN, GPIO_OVERRIDE_INVERT);
    gpio_pull_down(SPORT_PIN);  /* inverted idle level of a floating line */

    pio_sm_config c = sport_rx_program_get_default_config(off_rx);
    sm_config_set_in_pins(&c, SPORT_PIN);
    sm_config_set_in_shift(&c, true, true, 8);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&c, div);
    pio_sm_init(pio, sm_rx, off_rx, &c);

    c = sport_tx_program_get_default_config(off_tx);
    sm_config_set_out_pins(&c, SPORT_PIN, 1);
    sm_config_set_set_pins(&c, SPORT_PIN, 1);
    sm_config_set_sideset_pins(&c, SPORT_PIN);
    sm_config_set_out_shift(&c, true, false, 8);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&c, div);
    pio_sm_set_pins_with_mask(pio, sm_tx, 1u << SPORT_PIN, 1u << SPORT_PIN);
    pio_sm_set_pindirs_with_mask(pio, sm_tx, 0, 1u << SPORT_PIN);
    pio_sm_init(pio, sm_tx, off_tx, &c);

    pio_sm_set_enabled(pio, sm_rx, true);
    pio_sm_set_enabled(pio, sm_tx, true);
}

static bool rx_byte(uint8_t *b)
{
    if (pio_sm_is_rx_fifo_empty(pio, sm_rx))
        return false;
    *b = pio_sm_get(pio, sm_rx) >> 24;
    return true;
}

static void tx_frame(const uint8_t *raw, int n)
{
    uint32_t stall = 1u << (PIO_FDEBUG_TXSTALL_LSB + sm_tx);

    for (int i = 0; i < n; i++)
        pio_sm_put_blocking(pio, sm_tx, raw[i]);
    pio->fdebug = stall;
    while (!(pio->fdebug & stall))
        tight_loop_contents();
    /* the RX state machine heard us: drop it */
    sleep_us(1000000 / BAUDRATE);
    uint8_t b;
    while (rx_byte(&b))
        ;
}

static void reply(uint16_t id, int32_t value)
{
    uint8_t  p[8] = {DATA_FRAME, id & 0xFF, id >> 8, value & 0xFF, (value >> 8) & 0xFF,
                     (value >> 16) & 0xFF, (uint32_t)value >> 24, 0};
    uint8_t  raw[16];
    uint16_t sum = 0;
    int      n = 0;

    for (int i = 0; i < 7; i++) {
        sum += p[i];
        sum = (sum & 0xFF) + (sum >> 8);
    }
    p[7] = 0xFF - sum;
    for (int i = 0; i < 8; i++) {
        if (p[i] == START_BYTE || p[i] == STUFF_BYTE) {
            raw[n++] = STUFF_BYTE;
            raw[n++] = p[i] ^ STUFF_XOR;
        } else {
            raw[n++] = p[i];
        }
    }
    tx_frame(raw, n);
}

static void power_set(bool on)
{
#if POWER_PIN >= 0
    gpio_put(POWER_PIN, on);
#else
    (void)on;
#endif
}

/* A command line from USB, without blocking: true when one is complete */
static bool read_line(char *buf, int size)
{
    static int n;
    int        c;

    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (c == '\r' || c == '\n') {
            if (!n)
                continue;
            buf[n] = 0;
            n = 0;
            return true;
        }
        if (n < size - 1)
            buf[n++] = c;
    }
    return false;
}

int main(void)
{
    char     line[64];
    unsigned phys = 0;
    bool     quiet = false, start = false, alt_next = false, hold = false;
    uint32_t polls = 0, mine = 0, others = 0, replies = 0, seen = 0;
    uint32_t second;
    int32_t  vspeed = 0, alt = 0, dir = 10;

    stdio_init_all();
#if POWER_PIN >= 0
    gpio_init(POWER_PIN);
    gpio_set_dir(POWER_PIN, GPIO_OUT);
    power_set(true);
#endif
    line_init();
    second = time_us_32() + 1000000;

    for (;;) {
        uint8_t b;

        if (rx_byte(&b)) {
            uint32_t t = time_us_32();

            if (b == START_BYTE) {
                start = true;
            } else if (start) {
                start = false;
                polls++;
                if (b == sport_ids[phys]) {
                    mine++;
                    if (!quiet) {
                        while (time_us_32() - t < REPLY_US)
                            tight_loop_contents();
                        reply(alt_next ? ALT_ID : VARIO_ID, alt_next ? alt : vspeed);
                        alt_next = !alt_next;
                        replies++;
                    }
                } else {
                    for (unsigned i = 0; i < sizeof(sport_ids); i++)
                        if (b == sport_ids[i])
                            seen |= 1u << i;
                    others++;
                }
            }
        }

        if ((int32_t)(time_us_32() - second) >= 0) {
            second += 1000000;
            if (!hold) {
                vspeed += dir;
                if (vspeed >= 100)
                    dir = -10;
                else if (vspeed <= -100)
                    dir = 10;
            }
            alt += vspeed;
            printf("P polls=%lu mine=%lu replies=%lu others=%lu ids=%07lX vspeed=%ld alt=%ld\n",
                   (unsigned long)polls, (unsigned long)mine, (unsigned long)replies,
                   (unsigned long)others, (unsigned long)seen, (long)vspeed, (long)alt);
            polls = mine = others = replies = seen = 0;
        }

        if (read_line(line, sizeof(line))) {
            char *arg = strchr(line, ' ');
            int   v = arg ? atoi(arg + 1) : 0;

            if (!strncmp(line, "id", 2) && v >= 0 && v < (int)sizeof(sport_ids)) {
                phys = v;
                printf("OK id %u (poll byte %02X)\n", phys, sport_ids[phys]);
            } else if (!strncmp(line, "quiet", 5)) {
                quiet = v;
                printf("OK quiet %d\n", quiet);
            } else if (!strncmp(line, "hold", 4)) {
                hold = true;
                vspeed = v;
                printf("OK hold %ld\n", (long)vspeed);
            } else if (!strcmp(line, "ramp")) {
                hold = false;
                vspeed -= vspeed % 10;
                printf("OK ramp\n");
            } else if (!strncmp(line, "power", 5)) {
                power_set(v);
                printf("OK power %d\n", v);
            } else if (!strcmp(line, "bootsel")) {
                reset_usb_boot(0, 0);
            } else {
                printf("ERR %s\n", line);
            }
        }
    }
}
