#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

// Debug log, for users without a debugger: debug() / debugln() (debug.h)
// and the MPM's own debug lines into a ring of LOG_SIZE bytes, each line
// starting with the time ("  12.345 ", s and ms), read out by the board
// (mpm_port.c: telemetry frames on the USB link, tools/mpm_fs.py log).
//
// Always kept (the last LOG_SIZE bytes; tens of us a line: keep debug lines
// out of the busy paths), sent from power up with debug.txt ("usb = on",
// settings.c) or while a host asks for it, from the oldest line kept: the
// lines before a problem too. From the main loop only, not from interrupts.
#define LOG_SIZE 4096  // power of 2

#define LOG_FILE 0x01  // debug.txt
#define LOG_HOST 0x02  // a host's request

extern uint8_t log_on;  // sent for: LOG_FILE | LOG_HOST

// The time at each line start; none without it
void log_init(uint32_t (*millis)());

// Sending on (or off) for one source. A host turning it on reads from the
// oldest line kept
void log_enable(uint8_t source, bool on);

// printf's %d %i %u %x %X %c %s %p %%, with flags 0 and -, a width and l
// (ignored, 32 bits). On the CC2652: 15 us for "98 pps", 70 us for a
// protocol start line, 8 us a " %02X"
void log_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log_vprintf(const char* fmt, va_list ap);

// Up to max bytes not read yet into buf, their number. Bytes overwritten
// before they were read: "[N bytes lost]" instead
uint32_t log_read(char* buf, uint32_t max);

// Bytes not read yet
uint32_t log_pending();
