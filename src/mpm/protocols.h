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
#ifdef V2X2_NRF24L01_INO
void V2X2_init();
uint16_t V2X2_callback();
#endif
#ifdef XK_CCNRF_INO
void XK_init();
uint16_t XK_callback();
#endif
#ifdef DSM_CYRF6936_INO
void DSM_init();
uint16_t DSM_callback();
#endif
#ifdef FRSKYD_CC2500_INO
void FRSKYD_init();
uint16_t FRSKYD_callback();
#endif
#ifdef FRSKYV_CC2500_INO
void FRSKYV_init();
uint16_t FRSKYV_callback();
#endif
#ifdef FRSKYX_CC2500_INO
void FRSKYX_init();
uint16_t FRSKYX_callback();
#endif
#ifdef FLYSKY_A7105_INO
void FLYSKY_init();
uint16_t FLYSKY_callback();
#endif
#ifdef AFHDS2A_A7105_INO
void AFHDS2A_init();
uint16_t AFHDS2A_callback();
#endif
