#pragma once

#include "mpm.h"

// Protocol numbers of the serial link (Multiprotocol.h)
#define PROTO_TEST 127

void TEST_init();
uint32_t TEST_callback();
