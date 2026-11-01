#pragma once

// What the host tests share: CHECK() counting failures, hex() for packets
// written as hex strings, test_done() for main()'s summary and exit status

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>

static int fails;

#define CHECK(c)                                                       \
  do {                                                                 \
    if (!(c)) {                                                        \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);              \
      fails++;                                                         \
    }                                                                  \
  } while (0)

// Hex digits into b, up to the first other character: the bytes read
static inline int hex(const char* h, uint8_t* b)
{
  int i;
  for (i = 0; isxdigit((unsigned char)h[2 * i]) && isxdigit((unsigned char)h[2 * i + 1]); i++)
    sscanf(h + 2 * i, "%2hhx", &b[i]);
  return i;
}

// The last line of the output (the Makefile shows it), 1 if any failed
static inline int test_done(void)
{
  if (fails) printf("%d FAILED\n", fails);
  else printf("all ok\n");
  return fails != 0;
}
