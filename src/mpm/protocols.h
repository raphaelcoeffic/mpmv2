#pragma once

#include "mpm.h"
#include "upstream/mpm_config.h"
#include "upstream/multi_enums.h"  // protocol numbers (PROTO_TEST: the test protocol here)

void TEST_init();
uint16_t TEST_callback();

// Ported from the MPM (upstream/)
void end_bind_counter();

#ifdef BAYANG_NRF24L01_INO
void BAYANG_init();
uint16_t BAYANG_callback();
#endif
