#include "log.h"

#include <string.h>

uint8_t log_on;

static uint32_t (*_millis)();

static char _buf[LOG_SIZE];
// Bytes written and read since power up (wrap at 4 GB: differences only)
static uint32_t _head, _tail;
static bool _line_start = true;

void log_init(uint32_t (*millis)()) { _millis = millis; }

void log_enable(uint8_t source, bool on)
{
  if (!on) {
    log_on &= ~source;
    return;
  }
  if (source == LOG_HOST) {
    // from the oldest whole line kept
    uint32_t t = _head > LOG_SIZE ? _head - LOG_SIZE : 0;
    if (t) {
      while (t != _head && _buf[t & (LOG_SIZE - 1)] != '\n') t++;
      if (t != _head) t++;
    }
    _tail = t;
  }
  log_on |= source;
}

// Into the ring, the time at each line start ("  12.345 ")

static void _puts(const char* s, uint32_t n);

// Out of line: _puts() stays a few instructions a character
static void __attribute__((noinline)) _time()
{
  _line_start = false;
  if (!_millis) return;
  uint32_t ms = _millis();
  // from the end: " ", ms, ".", s, at least 9 characters
  char t[16];
  uint32_t e = sizeof(t), b = e;
  t[--b] = ' ';
  uint32_t v = ms % 1000;
  for (uint32_t i = 0; i < 3; i++, v /= 10) t[--b] = '0' + v % 10;
  t[--b] = '.';
  v = ms / 1000;
  do {
    t[--b] = '0' + v % 10;
    v /= 10;
  } while (v);
  while (e - b < 9) t[--b] = ' ';
  _puts(t + b, e - b);
}

// The ring position in a register: char stores alias the globals
static void _puts(const char* s, uint32_t n)
{
  uint32_t h = _head;
  bool start = _line_start;
  for (uint32_t i = 0; i < n; i++) {
    if (__builtin_expect(start, 0)) {
      _head = h;
      _time();
      h = _head;
    }
    char c = s[i];
    _buf[h++ & (LOG_SIZE - 1)] = c;
    start = c == '\n';
  }
  _head = h;
  _line_start = start;
}

static void _num(uint32_t v, bool neg, uint32_t base, bool upper, uint32_t width, bool zero,
                 bool left)
{
  // digits from the end of d, the padding before or after
  char d[32];
  uint32_t e = sizeof(d), b = e;
  const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  do {
    d[--b] = digits[v % base];
    v /= base;
  } while (v);
  if (width > 20) width = 20;
  if (zero) {
    while (e - b + neg < width) d[--b] = '0';
    if (neg) d[--b] = '-';
  } else {
    if (neg) d[--b] = '-';
    if (!left)
      while (e - b < width) d[--b] = ' ';
  }
  _puts(d + b, e - b);
  if (left)
    for (uint32_t n = e - b; n < width; n++) _puts(" ", 1);
}

void log_vprintf(const char* fmt, va_list ap)
{
  const char* f = fmt;
  while (*f) {
    const char* run = f;
    while (*f && *f != '%') f++;
    if (f != run) _puts(run, f - run);
    if (!*f) break;
    bool zero = false, left = false;
    uint32_t width = 0;
    for (f++; *f == '0' || *f == '-'; f++) {
      if (*f == '0') zero = true;
      else left = true;
    }
    for (; *f >= '0' && *f <= '9'; f++) width = width * 10 + (*f - '0');
    while (*f == 'l' || *f == 'h' || *f == 'z') f++;
    if (left) zero = false;
    switch (*f) {
      case 'd':
      case 'i': {
        int32_t v = va_arg(ap, int32_t);
        _num(v < 0 ? -(uint32_t)v : (uint32_t)v, v < 0, 10, false, width, zero, left);
        break;
      }
      case 'u':
        _num(va_arg(ap, uint32_t), false, 10, false, width, zero, left);
        break;
      case 'x':
      case 'X':
        _num(va_arg(ap, uint32_t), false, 16, *f == 'X', width, zero, left);
        break;
      case 'p':
        _puts("0x", 2);
        _num((uint32_t)(uintptr_t)va_arg(ap, void*), false, 16, false, 8, true, false);
        break;
      case 'c': {
        char c = (char)va_arg(ap, int);
        _puts(&c, 1);
        break;
      }
      case 's': {
        const char* s = va_arg(ap, const char*);
        if (!s) s = "(null)";
        uint32_t n = strlen(s);
        if (!left)
          for (; width > n; width--) _puts(" ", 1);
        _puts(s, n);
        for (; width > n; width--) _puts(" ", 1);
        break;
      }
      case 0:
        continue;
      default:
        _puts(f, 1);
        break;
    }
    f++;
  }
}

void log_printf(const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  log_vprintf(fmt, ap);
  va_end(ap);
}

uint32_t log_pending() { return _head - _tail > LOG_SIZE ? LOG_SIZE : _head - _tail; }

uint32_t log_read(char* buf, uint32_t max)
{
  if (_head - _tail > LOG_SIZE) {
    // "\n[N bytes lost]\n"
    uint32_t lost = _head - _tail - LOG_SIZE;
    char d[10];
    uint32_t k = 0;
    do {
      d[k++] = '0' + lost % 10;
      lost /= 10;
    } while (lost);
    const char* end = " bytes lost]\n";
    uint32_t n = 2 + k + strlen(end);
    if (n > max) return 0;
    char* p = buf;
    *p++ = '\n';
    *p++ = '[';
    while (k) *p++ = d[--k];
    memcpy(p, end, strlen(end));
    _tail = _head - LOG_SIZE;
    return n;
  }
  uint32_t n = _head - _tail;
  if (n > max) n = max;
  for (uint32_t i = 0; i < n; i++) buf[i] = _buf[(_tail + i) & (LOG_SIZE - 1)];
  _tail += n;
  return n;
}
