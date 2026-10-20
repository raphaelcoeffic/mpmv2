#pragma once

#include <stdbool.h>
#include <stdint.h>

// PPM mode: the model run with PPM input (no dial: one entry of the MPM's
// PPM_prot[] table). From the build's MPM_PPM_* (mpm.h), then ppm.txt on
// the MPM v2 (settings.c). Read for each PPM frame: a change applies at the
// next one (a protocol, sub protocol, rx_num or autobind change restarts
// the protocol)
typedef struct {
  uint8_t protocol;
  uint8_t sub_protocol;
  uint8_t rx_num;
  int8_t option;
  bool autobind;   // bind at each start (power up)
  bool low_power;
  uint16_t min_us;  // PPM widths for -100% and +100%
  uint16_t max_us;
} mpm_ppm_settings_t;

extern mpm_ppm_settings_t mpm_ppm;

// The build's settings
void mpm_ppm_defaults(mpm_ppm_settings_t* s);

// A settings text: "key = value" lines, '#' starts a comment, spaces around
// ignored. A token of it (not terminated)
typedef struct {
  const char* p;
  uint32_t len;
} mpm_tok_t;

// Error messages, "line N: what: token\n" each (terminated, cut to size)
typedef struct {
  char* p;
  uint32_t size, len;
  int count;  // errors, including those cut
} mpm_settings_err_t;

// A setting: NULL, what is wrong with the value, or MPM_SETTINGS_UNKNOWN_KEY
typedef const char* (*mpm_setting_fn_t)(void* ctx, mpm_tok_t key, mpm_tok_t val, uint32_t line);
extern const char MPM_SETTINGS_UNKNOWN_KEY[];

// fn for each line, an error for each bad one
void mpm_settings_lines(const char* text, uint32_t len, mpm_setting_fn_t fn, void* ctx,
                        mpm_settings_err_t* e);
// line 0: none; t.len 0: no token
void mpm_settings_error(mpm_settings_err_t* e, uint32_t line, const char* msg, mpm_tok_t t);

// Case and spaces ignored
bool mpm_tok_equal(mpm_tok_t t, const char* s);
// Decimal, up to 6 digits
bool mpm_tok_int(mpm_tok_t t, int32_t* v);
// 0 or 1, no or yes, off or on, false or true
bool mpm_tok_bool(mpm_tok_t t, bool* v);

// PPM settings text, "key = value" lines, '#' starts a comment:
//
//   protocol   name of the protocol table (FrSkyX2, FlSky2A...; case and
//              spaces ignored) or its number
//   sub        sub protocol name (LBT(EU), PWM,SBUS...) or number
//   rx_num     0 to 63
//   option     -128 to 127, or the byte 0 to 255 (AFHDS2A's 128...)
//   autobind   0 or 1 (also no/yes, off/on, false/true)
//   low_power  0 or 1
//   min_us     PPM width for -100% (500 to 2500)
//   max_us     for +100%, above min_us
//
// Only the keys given change s. A bad line changes nothing: one message per
// line into err ("line N: ...", terminated, cut to err_size), the number of
// bad lines returned
int mpm_ppm_parse(const char* text, uint32_t len, mpm_ppm_settings_t* s, char* err,
                  uint32_t err_size);
