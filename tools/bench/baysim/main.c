/*
 * Module bay simulator on an RP2040-Zero: stands in for a radio's external
 * module bay in front of the MPM v2 (docs/update.md, "Radio simulator").
 *
 * USB port "baysim link" (CDC 0) is a bridge to the bay lines: what the host
 * writes goes out on LINK_TX_PIN (to the bay's pin 1, PPM), what comes in on
 * LINK_RX_PIN (from the bay's pin 5, S.Port) goes to the host. The host's
 * line settings (baud rate, data bits, parity, stop bits) apply to the
 * UART, so radio_sim.py (100000 8E2) and stk500_flash.py (57600 8N1) run
 * unchanged. Or LINK_TX_PIN sends PPM (command ppm), from a PIO state
 * machine. Both lines are inverted in the GPIO block, as a radio's
 * external bay (invert 0: an internal module's UART).
 *
 * USB port "baysim ctrl" (CDC 1): text commands, one per line, each
 * answered by "ok ..." or "err ...":
 *
 *   ping                version
 *   pwr 0|1             the module supply (high-side switch on POWER_PIN)
 *   invert 0|1          line polarity (1: idle low, a radio's bay)
 *   tx drive|float      PPM pin driven (a radio) or left to the module's
 *                       pull-up (no radio)
 *   level               pad levels of both lines, before the inversion
 *   trace 0|1           event lines on this port: "P <us> 0|1" power,
 *                       "T <us> <hex>" sent, "R <us> <hex> [errors]"
 *                       received, the time of each group's first byte
 *   stats [0]           byte and error counts (0: and clear them)
 *   time                the microsecond clock of the trace
 *   ppm -|+ FRAME PULSE CH1 [CH2 ...]
 *                       PPM on the PPM pin instead of the UART, all times
 *                       in us: the frame length, the pulse length, the
 *                       channels (pulse included, 1 to 16 of them); -:
 *                       pulses low (EdgeTX's default), +: pulses high. A
 *                       new train starts at the end of the current frame
 *   ppm off             back to the UART
 *   ppm                 state, frames sent
 *   bootsel             reboot into the USB boot loader (also: the link
 *                       port opened at 1200 baud)
 *
 * The UART runs without FIFOs, one interrupt per byte, so the trace
 * timestamps are exact to a few microseconds.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/structs/io_bank0.h"
#include "hardware/uart.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "ppm.pio.h"
#include "tusb.h"

#define VERSION "baysim 2"

#ifndef LINK_UART
#define LINK_UART 0
#endif
#ifndef LINK_TX_PIN
#define LINK_TX_PIN 12
#endif
#ifndef LINK_RX_PIN
#define LINK_RX_PIN 13
#endif
#ifndef POWER_PIN
#define POWER_PIN 11
#endif

#define UART UART_INSTANCE(LINK_UART)
#define UART_IRQ (LINK_UART ? UART1_IRQ : UART0_IRQ)

#define CDC_LINK 0
#define CDC_CTRL 1

/* UART data register error bits */
#define DR_FE (1u << 8)
#define DR_PE (1u << 9)
#define DR_BE (1u << 10)
#define DR_OE (1u << 11)

/*
 * Events from the UART interrupt: received bytes (with their error bits)
 * and sent bytes, each with its time
 */
typedef struct {
    uint32_t t;
    uint16_t data;  // data and DR_* bits
    uint8_t tx;
} event_t;

#define EVENTS 4096  // power of two
static event_t events[EVENTS];
static volatile uint32_t ev_head, ev_tail;
static uint32_t ev_lost;

#define TX_BUF 1024  // power of two
static uint8_t tx_buf[TX_BUF];
static volatile uint32_t tx_head, tx_tail;

static bool invert = true;
static bool trace;
static bool tx_float;
static bool ppm_on;              // PPM on LINK_TX_PIN instead of the UART
static bool ppm_negative = true; // its pulses low
static uint32_t bits_per_char = 11;  // start + 8 + parity + 2 stop at 100k
static uint32_t baud = 100000;
static struct {
    uint32_t rx, tx, fe, pe, be, oe, lost;
} stats;

static void ev_push(uint32_t t, uint16_t data, bool tx)
{
    uint32_t h = ev_head;
    if (h - ev_tail >= EVENTS) {
        ev_lost++;
        return;
    }
    events[h & (EVENTS - 1)] = (event_t){t, data, tx};
    ev_head = h + 1;
}

static void uart_irq(void)
{
    uart_hw_t *hw = uart_get_hw(UART);
    uint32_t t = time_us_32();

    while (uart_is_readable(UART))
        ev_push(t, (uint16_t)(hw->dr & 0xFFF), false);
    // no FIFO: the holding register takes one byte at a time
    if (uart_is_writable(UART)) {
        if (tx_tail != tx_head) {
            uint8_t c = tx_buf[tx_tail & (TX_BUF - 1)];
            tx_tail++;
            hw->dr = c;
            ev_push(t, c, true);
        } else {
            hw_clear_bits(&hw->imsc, UART_UARTIMSC_TXIM_BITS);
        }
    }
}

static void tx_kick(void)
{
    uart_hw_t *hw = uart_get_hw(UART);
    irq_set_enabled(UART_IRQ, false);
    hw_set_bits(&hw->imsc, UART_UARTIMSC_TXIM_BITS);
    irq_set_enabled(UART_IRQ, true);
    // the TX interrupt only fires on a transition: start it by hand
    irq_set_pending(UART_IRQ);
}

/*
 * Lines and power
 */
static uint8_t pad_level(uint pin)
{
    return (io_bank0_hw->io[pin].status >> IO_BANK0_GPIO0_STATUS_INFROMPAD_LSB) & 1;
}

static void line_apply(void)
{
    if (ppm_on)
        gpio_set_outover(LINK_TX_PIN, ppm_negative ? GPIO_OVERRIDE_INVERT : GPIO_OVERRIDE_NORMAL);
    else
        gpio_set_outover(LINK_TX_PIN, invert ? GPIO_OVERRIDE_INVERT : GPIO_OVERRIDE_NORMAL);
    gpio_set_inover(LINK_RX_PIN, invert ? GPIO_OVERRIDE_INVERT : GPIO_OVERRIDE_NORMAL);
    gpio_set_oeover(LINK_TX_PIN, tx_float ? GPIO_OVERRIDE_LOW : GPIO_OVERRIDE_NORMAL);
    // S.Port is pulled up on the module (R22): no pull here
    gpio_disable_pulls(LINK_RX_PIN);
}

static void line_format(uint32_t rate, uint data_bits, uint stop_bits, uart_parity_t parity)
{
    baud = uart_set_baudrate(UART, rate);
    uart_set_format(UART, data_bits, stop_bits, parity);
    uart_set_fifo_enabled(UART, false);
    bits_per_char = 1 + data_bits + (parity != UART_PARITY_NONE) + stop_bits;
}

static void line_init(void)
{
    uart_init(UART, baud);
    line_format(100000, 8, 2, UART_PARITY_EVEN);
    uart_set_hw_flow(UART, false, false);
    gpio_set_function(LINK_TX_PIN, UART_FUNCSEL_NUM(UART, LINK_TX_PIN));
    gpio_set_function(LINK_RX_PIN, UART_FUNCSEL_NUM(UART, LINK_RX_PIN));
    // gpio_set_function() clears the overrides: set them afterwards
    line_apply();

    irq_set_exclusive_handler(UART_IRQ, uart_irq);
    irq_set_priority(UART_IRQ, PICO_HIGHEST_IRQ_PRIORITY);
    uart_set_irqs_enabled(UART, true, false);
    irq_set_enabled(UART_IRQ, true);
}

/*
 * PPM: the state machine takes one word per pulse (ppm.pio), the FIFO
 * refilled from the interrupt, from one of two trains: the other one is
 * written by the ppm command and taken at the next frame start
 */
#define PPM_PIO pio0
#define PPM_SM 0
#define PPM_IRQ PIO0_IRQ_0
#define PPM_MAX_CH 16

static uint32_t ppm_words[2][PPM_MAX_CH + 1];
static uint32_t ppm_len[2];
static volatile int ppm_cur, ppm_next = -1;
static uint32_t ppm_pos;
static volatile uint32_t ppm_frames;
static int ppm_offset = -1;

static void ppm_irq(void)
{
    while (!pio_sm_is_tx_fifo_full(PPM_PIO, PPM_SM)) {
        if (!ppm_pos && ppm_next >= 0) {
            ppm_cur = ppm_next;
            ppm_next = -1;
        }
        pio_sm_put(PPM_PIO, PPM_SM, ppm_words[ppm_cur][ppm_pos]);
        if (++ppm_pos == ppm_len[ppm_cur]) {
            ppm_pos = 0;
            ppm_frames++;
        }
    }
}

static uint32_t ppm_word(uint32_t pulse, uint32_t space)
{
    return (pulse - 2) << 16 | (space - 5);
}

// The train for the next frame start; false if the times don't fit
static bool ppm_set(uint32_t frame, uint32_t pulse, const uint32_t *ch, uint32_t n)
{
    uint32_t sum = 0;
    if (!n || n > PPM_MAX_CH || pulse < 50 || pulse > 1000)
        return false;
    for (uint32_t i = 0; i < n; i++) {
        if (ch[i] < pulse + 50 || ch[i] > 60000)
            return false;
        sum += ch[i];
    }
    if (frame < sum + pulse + 50 || frame - sum > 60000)
        return false;

    irq_set_enabled(PPM_IRQ, false);
    int b = ppm_on ? 1 - ppm_cur : 0;
    for (uint32_t i = 0; i < n; i++)
        ppm_words[b][i] = ppm_word(pulse, ch[i] - pulse);
    ppm_words[b][n] = ppm_word(pulse, frame - sum - pulse);
    ppm_len[b] = n + 1;
    if (ppm_on) {
        ppm_next = b;
    } else {
        ppm_cur = b;
        ppm_next = -1;
        ppm_pos = 0;
    }
    irq_set_enabled(PPM_IRQ, ppm_on);
    return true;
}

static void ppm_start(void)
{
    if (ppm_offset < 0)
        ppm_offset = pio_add_program(PPM_PIO, &ppm_program);
    pio_sm_config c = ppm_program_get_default_config(ppm_offset);
    sm_config_set_set_pins(&c, LINK_TX_PIN, 1);
    // x (the space) from the low half
    sm_config_set_out_shift(&c, true, false, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / 1000000.0f);
    pio_sm_init(PPM_PIO, PPM_SM, ppm_offset, &c);
    pio_sm_set_pins_with_mask(PPM_PIO, PPM_SM, 0, 1u << LINK_TX_PIN);
    pio_sm_set_consecutive_pindirs(PPM_PIO, PPM_SM, LINK_TX_PIN, 1, true);
    ppm_frames = 0;
    ppm_on = true;
    // gpio_set_function() clears the overrides: set them afterwards
    pio_gpio_init(PPM_PIO, LINK_TX_PIN);
    line_apply();

    ppm_irq();
    irq_set_exclusive_handler(PPM_IRQ, ppm_irq);
    pio_set_irq0_source_enabled(PPM_PIO, pis_sm0_tx_fifo_not_full, true);
    irq_set_enabled(PPM_IRQ, true);
    pio_sm_set_enabled(PPM_PIO, PPM_SM, true);
}

static void ppm_stop(void)
{
    irq_set_enabled(PPM_IRQ, false);
    pio_set_irq0_source_enabled(PPM_PIO, pis_sm0_tx_fifo_not_full, false);
    pio_sm_set_enabled(PPM_PIO, PPM_SM, false);
    pio_sm_clear_fifos(PPM_PIO, PPM_SM);
    ppm_on = false;
    ppm_next = -1;
    gpio_set_function(LINK_TX_PIN, UART_FUNCSEL_NUM(UART, LINK_TX_PIN));
    line_apply();
}

/*
 * USB output: never blocks for long, counts what it had to drop
 */
static uint32_t ctrl_dropped, link_dropped;

static bool cdc_write(uint8_t itf, const void *buf, uint32_t len)
{
    uint32_t start = time_us_32();

    if (!tud_cdc_n_connected(itf))
        return false;
    while (tud_cdc_n_write_available(itf) < len) {
        tud_task();
        tud_cdc_n_write_flush(itf);
        if (time_us_32() - start > 20000 || !tud_cdc_n_connected(itf))
            return false;
    }
    tud_cdc_n_write(itf, buf, len);
    return true;
}

static void ctrl_printf(const char *fmt, ...)
{
    char line[300];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(line) - 3)
        n = sizeof(line) - 3;
    line[n++] = '\n';
    if (!cdc_write(CDC_CTRL, line, n))
        ctrl_dropped++;
}

/*
 * Event groups for the trace: the bytes of one direction until a gap of
 * two characters, or 64 bytes
 */
typedef struct {
    uint32_t t, last, n;
    uint32_t fe, pe, be, oe;
    char hex[2 * 64 + 1];
} group_t;

static group_t grp[2];  // 0 received, 1 sent

static void group_flush(int tx)
{
    group_t *g = &grp[tx];
    if (!g->n)
        return;
    if (g->fe || g->pe || g->be || g->oe)
        ctrl_printf("%c %lu %s fe=%lu pe=%lu be=%lu oe=%lu", tx ? 'T' : 'R', (unsigned long)g->t, g->hex,
                    (unsigned long)g->fe, (unsigned long)g->pe, (unsigned long)g->be, (unsigned long)g->oe);
    else
        ctrl_printf("%c %lu %s", tx ? 'T' : 'R', (unsigned long)g->t, g->hex);
    memset(g, 0, sizeof(*g));
}

static void group_add(const event_t *e)
{
    static const char digits[] = "0123456789abcdef";
    group_t *g = &grp[e->tx];

    if (g->n == 64)
        group_flush(e->tx);
    if (!g->n)
        g->t = e->t;
    g->last = e->t;
    g->hex[2 * g->n] = digits[(e->data >> 4) & 15];
    g->hex[2 * g->n + 1] = digits[e->data & 15];
    g->hex[2 * g->n + 2] = 0;
    g->n++;
    g->fe += !!(e->data & DR_FE);
    g->pe += !!(e->data & DR_PE);
    g->be += !!(e->data & DR_BE);
    g->oe += !!(e->data & DR_OE);
}

static void events_pump(void)
{
    uint8_t rx[64];
    uint32_t n = 0;
    uint32_t gap = 2 * bits_per_char * 1000000u / baud;

    while (ev_tail != ev_head) {
        event_t e = events[ev_tail & (EVENTS - 1)];
        ev_tail++;
        if (e.tx) {
            stats.tx++;
        } else {
            stats.rx++;
            stats.fe += !!(e.data & DR_FE);
            stats.pe += !!(e.data & DR_PE);
            stats.be += !!(e.data & DR_BE);
            stats.oe += !!(e.data & DR_OE);
            // a break is not a byte
            if (!(e.data & DR_BE)) {
                rx[n++] = (uint8_t)e.data;
                if (n == sizeof(rx)) {
                    if (!cdc_write(CDC_LINK, rx, n))
                        link_dropped += n;
                    n = 0;
                }
            }
        }
        if (trace) {
            group_t *g = &grp[e.tx];
            if (g->n && e.t - g->last > gap)
                group_flush(e.tx);
            group_add(&e);
        }
    }
    if (n && !cdc_write(CDC_LINK, rx, n))
        link_dropped += n;
    if (trace) {
        uint32_t now = time_us_32();
        for (int i = 0; i < 2; i++)
            if (grp[i].n && now - grp[i].last > gap)
                group_flush(i);
    }
    if (ev_lost) {
        stats.lost += ev_lost;
        ev_lost = 0;
    }
}

static void link_pump(void)
{
    uint8_t buf[64];
    uint32_t space = TX_BUF - (tx_head - tx_tail);
    uint32_t n;

    if (space < sizeof(buf) || !tud_cdc_n_available(CDC_LINK))
        return;
    n = tud_cdc_n_read(CDC_LINK, buf, sizeof(buf));
    for (uint32_t i = 0; i < n; i++)
        tx_buf[(tx_head + i) & (TX_BUF - 1)] = buf[i];
    tx_head += n;
    if (n)
        tx_kick();
}

/* The host's line settings on the link port */
void tud_cdc_line_coding_cb(uint8_t itf, const cdc_line_coding_t *lc)
{
    static const uart_parity_t parities[] = {UART_PARITY_NONE, UART_PARITY_ODD, UART_PARITY_EVEN};

    if (itf != CDC_LINK)
        return;
    if (lc->bit_rate == 1200)
        reset_usb_boot(0, 0);
    if (lc->bit_rate < 1200 || lc->data_bits < 5 || lc->data_bits > 8 || lc->parity > 2)
        return;
    line_format(lc->bit_rate, lc->data_bits, lc->stop_bits == 2 ? 2 : 1, parities[lc->parity]);
}

/*
 * Control commands
 */
static void power_set(bool on)
{
    gpio_put(POWER_PIN, on);
    if (trace)
        ctrl_printf("P %lu %d", (unsigned long)time_us_32(), on);
}

static void command(char *line)
{
    char *argv[4 + PPM_MAX_CH];
    int argc = 0;

    for (char *p = strtok(line, " \t"); p && argc < (int)(sizeof(argv) / sizeof(argv[0]));
         p = strtok(NULL, " \t"))
        argv[argc++] = p;
    if (!argc)
        return;

    if (!strcmp(argv[0], "ping")) {
        ctrl_printf("ok %s", VERSION);
    } else if (!strcmp(argv[0], "pwr") && argc == 2) {
        power_set(atoi(argv[1]) != 0);
        ctrl_printf("ok pwr=%d t=%lu", gpio_get_out_level(POWER_PIN), (unsigned long)time_us_32());
    } else if (!strcmp(argv[0], "invert") && argc == 2) {
        invert = atoi(argv[1]) != 0;
        line_apply();
        ctrl_printf("ok invert=%d", invert);
    } else if (!strcmp(argv[0], "tx") && argc == 2 && (!strcmp(argv[1], "drive") || !strcmp(argv[1], "float"))) {
        tx_float = !strcmp(argv[1], "float");
        line_apply();
        ctrl_printf("ok tx=%s", argv[1]);
    } else if (!strcmp(argv[0], "level")) {
        ctrl_printf("ok tx=%d rx=%d pwr=%d", pad_level(LINK_TX_PIN), pad_level(LINK_RX_PIN),
                    gpio_get_out_level(POWER_PIN));
    } else if (!strcmp(argv[0], "trace") && argc == 2) {
        trace = atoi(argv[1]) != 0;
        memset(grp, 0, sizeof(grp));
        ctrl_printf("ok trace=%d", trace);
    } else if (!strcmp(argv[0], "stats")) {
        ctrl_printf("ok rx=%lu tx=%lu fe=%lu pe=%lu be=%lu oe=%lu lost=%lu link_dropped=%lu ctrl_dropped=%lu"
                    " baud=%lu bits=%lu",
                    (unsigned long)stats.rx, (unsigned long)stats.tx, (unsigned long)stats.fe,
                    (unsigned long)stats.pe, (unsigned long)stats.be, (unsigned long)stats.oe,
                    (unsigned long)stats.lost, (unsigned long)link_dropped, (unsigned long)ctrl_dropped,
                    (unsigned long)baud, (unsigned long)bits_per_char);
        if (argc == 2 && !strcmp(argv[1], "0")) {
            memset(&stats, 0, sizeof(stats));
            link_dropped = ctrl_dropped = 0;
        }
    } else if (!strcmp(argv[0], "time")) {
        ctrl_printf("ok t=%lu", (unsigned long)time_us_32());
    } else if (!strcmp(argv[0], "ppm") && argc == 2 && !strcmp(argv[1], "off")) {
        if (ppm_on)
            ppm_stop();
        ctrl_printf("ok ppm=off");
    } else if (!strcmp(argv[0], "ppm") && argc >= 5 && (!strcmp(argv[1], "-") || !strcmp(argv[1], "+"))) {
        uint32_t ch[PPM_MAX_CH];
        uint32_t n = argc - 4;
        for (uint32_t i = 0; i < n; i++)
            ch[i] = strtoul(argv[4 + i], NULL, 10);
        bool negative = !strcmp(argv[1], "-");
        if (ppm_on && negative != ppm_negative)
            ppm_stop();
        if (!ppm_set(strtoul(argv[2], NULL, 10), strtoul(argv[3], NULL, 10), ch, n)) {
            ctrl_printf("err ppm times");
        } else {
            ppm_negative = negative;
            if (!ppm_on)
                ppm_start();
            ctrl_printf("ok ppm=%s n=%lu", argv[1], (unsigned long)n);
        }
    } else if (!strcmp(argv[0], "ppm") && argc == 1) {
        ctrl_printf("ok ppm=%s frames=%lu", ppm_on ? (ppm_negative ? "-" : "+") : "off",
                    (unsigned long)ppm_frames);
    } else if (!strcmp(argv[0], "bootsel")) {
        ctrl_printf("ok");
        tud_cdc_n_write_flush(CDC_CTRL);
        sleep_ms(50);
        reset_usb_boot(0, 0);
    } else {
        ctrl_printf("err %s", argv[0]);
    }
}

static void ctrl_pump(void)
{
    static char line[128];
    static uint32_t len;
    char c;

    while (tud_cdc_n_available(CDC_CTRL) && tud_cdc_n_read(CDC_CTRL, &c, 1) == 1) {
        if (c == '\r' || c == '\n') {
            line[len] = 0;
            command(line);
            len = 0;
        } else if (len < sizeof(line) - 1) {
            line[len++] = c;
        }
    }
}

int main(void)
{
    gpio_init(POWER_PIN);
    gpio_put(POWER_PIN, 0);
    gpio_set_dir(POWER_PIN, GPIO_OUT);
    line_init();
    tud_init(0);

    for (;;) {
        tud_task();
        events_pump();
        link_pump();
        ctrl_pump();
        tud_cdc_n_write_flush(CDC_LINK);
        tud_cdc_n_write_flush(CDC_CTRL);
    }
}
