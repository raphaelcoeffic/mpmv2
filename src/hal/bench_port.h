#pragma once

// What the radio bench (bench.c) needs from the board (the board's
// bench_port.c): the chip's name, interrupts off for a few lines, the RF
// activity LED, and the RF core's PA / LNA enable edges for "timing"

#include <stdbool.h>
#include <stdint.h>

// The chip the bench drives: "info" (dev=), "chip"
extern const char bench_port_dev[];

// Interrupts off; true if they were off already (bench_port_irq_enable()
// only if not)
bool bench_port_irq_disable();
void bench_port_irq_enable();

typedef enum { BENCH_LED_IDLE, BENCH_LED_TX, BENCH_LED_RX, BENCH_LED_RX_BAD } bench_led_t;

void bench_port_led(bench_led_t state);

// Edges of the RF core's PA enable (BENCH_EDGE_PA) and LNA enable
// (BENCH_EDGE_LNA), timestamped in an interrupt (get_ticks()) and passed to
// bench_edge()
#define BENCH_EDGE_PA 0
#define BENCH_EDGE_LNA 1

// Edge capture on, or off (the pins as before). False: no PA enable pin on
// this board
bool bench_port_timing(bool on);
// The LNA enable captured too
bool bench_port_timing_lna();

// bench.c: an edge, from the port's interrupt
void bench_edge(uint32_t ticks, uint8_t signal, uint8_t level);
