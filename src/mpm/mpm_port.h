#pragma once

// What the MULTI-Module core needs from the board (mpm_port.c) or from the
// host tests

#include <stdbool.h>
#include <stdint.h>

uint32_t mpm_port_micros();
uint32_t mpm_port_millis();

// Link with the radio
void mpm_port_init();
// Latest frame received from the radio, if any since the last call
bool mpm_port_read_frame(uint8_t* buf, uint8_t* len);
// Latest PPM frame, if any since the last call: the channel widths in us
// (MPM v2 module bay)
bool mpm_port_read_ppm(uint16_t* us, uint8_t* n);
// Telemetry to the radio
void mpm_port_write(const uint8_t* data, uint8_t len);
// Room left for telemetry (bytes)
uint16_t mpm_port_write_free();

void mpm_port_led(bool on);

// Protocol change: stop the RF chips (as modules_reset())
void mpm_port_reset_rf();

// Module id: protocol addresses derive from it (+ rx_num)
uint32_t mpm_port_module_id();

// Board work after each poll (the EEPROM written to the flash). A long job
// calls mpm_poll() while it waits: the callbacks and the input go on, this
// is not called again until it returns
void mpm_port_idle();
