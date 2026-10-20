#include "mpm_settings.h"
#include "mpm.h"

#include <string.h>

mpm_ppm_settings_t mpm_ppm = {
    .protocol = MPM_PPM_PROTOCOL,
    .sub_protocol = MPM_PPM_SUB_PROTOCOL,
    .rx_num = MPM_PPM_RX_NUM,
    .option = MPM_PPM_OPTION,
    .min_us = MPM_PPM_MIN_100,
    .max_us = MPM_PPM_MAX_100,
};

void mpm_ppm_defaults(mpm_ppm_settings_t* s)
{
  memset(s, 0, sizeof(*s));
  s->protocol = MPM_PPM_PROTOCOL;
  s->sub_protocol = MPM_PPM_SUB_PROTOCOL;
  s->rx_num = MPM_PPM_RX_NUM;
  s->option = MPM_PPM_OPTION;
  s->min_us = MPM_PPM_MIN_100;
  s->max_us = MPM_PPM_MAX_100;
}

// A token of the text: not terminated
typedef struct {
  const char* p;
  uint32_t len;
} tok_t;

static char _lower(char c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }

// A name of the tables (up to n characters, '\0' or space padded) against a
// token: case and spaces ignored
static bool _name_equal(const char* name, uint32_t n, tok_t t)
{
  uint32_t i = 0, j = 0;
  for (;;) {
    while (i < n && name[i] == ' ') i++;
    while (j < t.len && t.p[j] == ' ') j++;
    bool name_end = i >= n || name[i] == 0;
    bool tok_end = j >= t.len;
    if (name_end || tok_end) return name_end && tok_end;
    if (_lower(name[i++]) != _lower(t.p[j++])) return false;
  }
}

static bool _tok_equal(tok_t t, const char* s) { return _name_equal(s, strlen(s), t); }

static bool _int(tok_t t, int32_t* v)
{
  uint32_t i = 0;
  bool neg = false;
  if (i < t.len && (t.p[i] == '-' || t.p[i] == '+')) neg = t.p[i++] == '-';
  if (i == t.len || t.len - i > 6) return false;
  int32_t r = 0;
  for (; i < t.len; i++) {
    if (t.p[i] < '0' || t.p[i] > '9') return false;
    r = r * 10 + (t.p[i] - '0');
  }
  *v = neg ? -r : r;
  return true;
}

static bool _bool(tok_t t, bool* v)
{
  if (_tok_equal(t, "1") || _tok_equal(t, "yes") || _tok_equal(t, "on") || _tok_equal(t, "true")) {
    *v = true;
    return true;
  }
  if (_tok_equal(t, "0") || _tok_equal(t, "no") || _tok_equal(t, "off") || _tok_equal(t, "false")) {
    *v = false;
    return true;
  }
  return false;
}

static const mpm_protocol_t* _protocol(uint8_t id)
{
  for (uint8_t i = 0; i < mpm_protocol_count; i++)
    if (mpm_protocols[i].id == id) return &mpm_protocols[i];
  return 0;
}

static const mpm_protocol_t* _protocol_named(tok_t t)
{
  int32_t v;
  if (_int(t, &v)) return v >= 0 && v <= 255 ? _protocol(v) : 0;
  for (uint8_t i = 0; i < mpm_protocol_count; i++)
    if (_name_equal(mpm_protocols[i].name, strlen(mpm_protocols[i].name), t)) return &mpm_protocols[i];
  return 0;
}

// Sub protocol of p by name or number, -1 if none
static int _sub_named(const mpm_protocol_t* p, tok_t t)
{
  int32_t v;
  if (_int(t, &v)) return v >= 0 && v < (p->n_sub ? p->n_sub : 1) ? v : -1;
  if (!p->sub_names) return -1;
  uint8_t n = p->sub_names[0];
  for (uint8_t i = 0; i < p->n_sub; i++)
    if (_name_equal(p->sub_names + 1 + i * n, n, t)) return i;
  return -1;
}

static tok_t _trim(const char* p, const char* end)
{
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
  while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
  return (tok_t){p, (uint32_t)(end - p)};
}

// Error messages
typedef struct {
  char* p;
  uint32_t size, len;
} err_t;

static void _put(err_t* e, const char* s, uint32_t n)
{
  while (n-- && e->len + 1 < e->size) e->p[e->len++] = *s++;
  if (e->size) e->p[e->len] = 0;
}

// line 0: none
static void _error(err_t* e, uint32_t line, const char* msg, tok_t t)
{
  if (line) {
    char num[12];
    uint32_t n = sizeof(num);
    do {
      num[--n] = '0' + line % 10;
      line /= 10;
    } while (line);
    _put(e, "line ", 5);
    _put(e, num + n, sizeof(num) - n);
    _put(e, ": ", 2);
  }
  _put(e, msg, strlen(msg));
  if (t.len) {
    _put(e, ": ", 2);
    _put(e, t.p, t.len > 32 ? 32 : t.len);
  }
  _put(e, "\n", 1);
}

int mpm_ppm_parse(const char* text, uint32_t len, mpm_ppm_settings_t* s, char* err,
                  uint32_t err_size)
{
  err_t e = {err, err_size, 0};
  if (err_size) err[0] = 0;
  int errors = 0;
  // the sub protocol once the protocol is known (any order in the text)
  tok_t sub = {0, 0};
  uint32_t sub_line = 0;
  mpm_ppm_settings_t r = *s;

  const char* end = text + len;
  uint32_t line = 0;
  for (const char* p = text; p < end;) {
    const char* eol = memchr(p, '\n', end - p);
    if (!eol) eol = end;
    line++;
    const char* hash = memchr(p, '#', eol - p);
    tok_t l = _trim(p, hash ? hash : eol);
    p = eol + 1;
    if (l.len == 0) continue;

    const char* eq = memchr(l.p, '=', l.len);
    if (!eq) {
      _error(&e, line, "not key = value", l);
      errors++;
      continue;
    }
    tok_t key = _trim(l.p, eq);
    tok_t val = _trim(eq + 1, l.p + l.len);
    int32_t v;
    bool b;
    const char* bad = 0;
    if (_tok_equal(key, "protocol")) {
      const mpm_protocol_t* proto = _protocol_named(val);
      if (proto) r.protocol = proto->id;
      else bad = "unknown protocol";
    } else if (_tok_equal(key, "sub")) {
      sub = val;
      sub_line = line;
    } else if (_tok_equal(key, "rx_num")) {
      if (_int(val, &v) && v >= 0 && v <= 63) r.rx_num = v;
      else bad = "rx_num: 0 to 63";
    } else if (_tok_equal(key, "option")) {
      if (_int(val, &v) && v >= -128 && v <= 255) r.option = (int8_t)v;
      else bad = "option: -128 to 255";
    } else if (_tok_equal(key, "autobind")) {
      if (_bool(val, &b)) r.autobind = b;
      else bad = "autobind: 0 or 1";
    } else if (_tok_equal(key, "low_power")) {
      if (_bool(val, &b)) r.low_power = b;
      else bad = "low_power: 0 or 1";
    } else if (_tok_equal(key, "min_us")) {
      if (_int(val, &v) && v >= 500 && v <= 2500) r.min_us = v;
      else bad = "min_us: 500 to 2500";
    } else if (_tok_equal(key, "max_us")) {
      if (_int(val, &v) && v >= 500 && v <= 2500) r.max_us = v;
      else bad = "max_us: 500 to 2500";
    } else {
      _error(&e, line, "unknown key", key);
      errors++;
      continue;
    }
    if (bad) {
      _error(&e, line, bad, val);
      errors++;
    }
  }

  const mpm_protocol_t* proto = _protocol(r.protocol);
  if (sub.p) {
    int i = proto ? _sub_named(proto, sub) : -1;
    if (i >= 0) {
      r.sub_protocol = i;
    } else {
      _error(&e, sub_line, "no such sub protocol", sub);
      errors++;
    }
  } else if (r.protocol != s->protocol) {
    // a new protocol without its sub protocol: the first one
    r.sub_protocol = 0;
  }
  if (r.min_us >= r.max_us) {
    _error(&e, 0, "min_us must be below max_us", (tok_t){0, 0});
    errors++;
    r.min_us = s->min_us;
    r.max_us = s->max_us;
  }
  *s = r;
  return errors;
}
