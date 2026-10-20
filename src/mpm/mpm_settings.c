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

const char MPM_SETTINGS_UNKNOWN_KEY[] = "unknown key";

static char _lower(char c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }

// A name of the tables (up to n characters, '\0' or space padded) against a
// token: case and spaces ignored
static bool _name_equal(const char* name, uint32_t n, mpm_tok_t t)
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

bool mpm_tok_equal(mpm_tok_t t, const char* s) { return _name_equal(s, strlen(s), t); }

bool mpm_tok_int(mpm_tok_t t, int32_t* v)
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

bool mpm_tok_bool(mpm_tok_t t, bool* v)
{
  if (mpm_tok_equal(t, "1") || mpm_tok_equal(t, "yes") || mpm_tok_equal(t, "on") ||
      mpm_tok_equal(t, "true")) {
    *v = true;
    return true;
  }
  if (mpm_tok_equal(t, "0") || mpm_tok_equal(t, "no") || mpm_tok_equal(t, "off") ||
      mpm_tok_equal(t, "false")) {
    *v = false;
    return true;
  }
  return false;
}

static mpm_tok_t _trim(const char* p, const char* end)
{
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
  while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
  return (mpm_tok_t){p, (uint32_t)(end - p)};
}

static void _put(mpm_settings_err_t* e, const char* s, uint32_t n)
{
  while (n-- && e->len + 1 < e->size) e->p[e->len++] = *s++;
  if (e->size) e->p[e->len] = 0;
}

void mpm_settings_error(mpm_settings_err_t* e, uint32_t line, const char* msg, mpm_tok_t t)
{
  e->count++;
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

void mpm_settings_lines(const char* text, uint32_t len, mpm_setting_fn_t fn, void* ctx,
                        mpm_settings_err_t* e)
{
  const char* end = text + len;
  uint32_t line = 0;
  for (const char* p = text; p < end;) {
    const char* eol = memchr(p, '\n', end - p);
    if (!eol) eol = end;
    line++;
    const char* hash = memchr(p, '#', eol - p);
    mpm_tok_t l = _trim(p, hash ? hash : eol);
    p = eol + 1;
    if (l.len == 0) continue;

    const char* eq = memchr(l.p, '=', l.len);
    if (!eq) {
      mpm_settings_error(e, line, "not key = value", l);
      continue;
    }
    mpm_tok_t key = _trim(l.p, eq);
    mpm_tok_t val = _trim(eq + 1, l.p + l.len);
    const char* bad = fn(ctx, key, val, line);
    if (bad == MPM_SETTINGS_UNKNOWN_KEY) mpm_settings_error(e, line, bad, key);
    else if (bad) mpm_settings_error(e, line, bad, val);
  }
}

static const mpm_protocol_t* _protocol(uint8_t id)
{
  for (uint8_t i = 0; i < mpm_protocol_count; i++)
    if (mpm_protocols[i].id == id) return &mpm_protocols[i];
  return 0;
}

static const mpm_protocol_t* _protocol_named(mpm_tok_t t)
{
  int32_t v;
  if (mpm_tok_int(t, &v)) return v >= 0 && v <= 255 ? _protocol(v) : 0;
  for (uint8_t i = 0; i < mpm_protocol_count; i++)
    if (_name_equal(mpm_protocols[i].name, strlen(mpm_protocols[i].name), t)) return &mpm_protocols[i];
  return 0;
}

// Sub protocol of p by name or number, -1 if none
static int _sub_named(const mpm_protocol_t* p, mpm_tok_t t)
{
  int32_t v;
  if (mpm_tok_int(t, &v)) return v >= 0 && v < (p->n_sub ? p->n_sub : 1) ? v : -1;
  if (!p->sub_names) return -1;
  uint8_t n = p->sub_names[0];
  for (uint8_t i = 0; i < p->n_sub; i++)
    if (_name_equal(p->sub_names + 1 + i * n, n, t)) return i;
  return -1;
}

typedef struct {
  mpm_ppm_settings_t r;
  // the sub protocol once the protocol is known (any order in the text)
  mpm_tok_t sub;
  uint32_t sub_line;
} ppm_ctx_t;

static const char* _ppm_setting(void* ctx, mpm_tok_t key, mpm_tok_t val, uint32_t line)
{
  ppm_ctx_t* c = ctx;
  mpm_ppm_settings_t* r = &c->r;
  int32_t v;
  bool b;
  if (mpm_tok_equal(key, "protocol")) {
    const mpm_protocol_t* proto = _protocol_named(val);
    if (!proto) return "unknown protocol";
    r->protocol = proto->id;
  } else if (mpm_tok_equal(key, "sub")) {
    c->sub = val;
    c->sub_line = line;
  } else if (mpm_tok_equal(key, "rx_num")) {
    if (!mpm_tok_int(val, &v) || v < 0 || v > 63) return "rx_num: 0 to 63";
    r->rx_num = v;
  } else if (mpm_tok_equal(key, "option")) {
    if (!mpm_tok_int(val, &v) || v < -128 || v > 255) return "option: -128 to 255";
    r->option = (int8_t)v;
  } else if (mpm_tok_equal(key, "autobind")) {
    if (!mpm_tok_bool(val, &b)) return "autobind: 0 or 1";
    r->autobind = b;
  } else if (mpm_tok_equal(key, "low_power")) {
    if (!mpm_tok_bool(val, &b)) return "low_power: 0 or 1";
    r->low_power = b;
  } else if (mpm_tok_equal(key, "min_us")) {
    if (!mpm_tok_int(val, &v) || v < 500 || v > 2500) return "min_us: 500 to 2500";
    r->min_us = v;
  } else if (mpm_tok_equal(key, "max_us")) {
    if (!mpm_tok_int(val, &v) || v < 500 || v > 2500) return "max_us: 500 to 2500";
    r->max_us = v;
  } else {
    return MPM_SETTINGS_UNKNOWN_KEY;
  }
  return 0;
}

int mpm_ppm_parse(const char* text, uint32_t len, mpm_ppm_settings_t* s, char* err,
                  uint32_t err_size)
{
  mpm_settings_err_t e = {err, err_size, 0, 0};
  if (err_size) err[0] = 0;
  ppm_ctx_t c = {.r = *s};
  mpm_settings_lines(text, len, _ppm_setting, &c, &e);

  mpm_ppm_settings_t* r = &c.r;
  const mpm_protocol_t* proto = _protocol(r->protocol);
  if (c.sub.p) {
    int i = proto ? _sub_named(proto, c.sub) : -1;
    if (i >= 0) r->sub_protocol = i;
    else mpm_settings_error(&e, c.sub_line, "no such sub protocol", c.sub);
  } else if (r->protocol != s->protocol) {
    // a new protocol without its sub protocol: the first one
    r->sub_protocol = 0;
  }
  if (r->min_us >= r->max_us) {
    mpm_settings_error(&e, 0, "min_us must be below max_us", (mpm_tok_t){0, 0});
    r->min_us = s->min_us;
    r->max_us = s->max_us;
  }
  *s = *r;
  return e.count;
}
