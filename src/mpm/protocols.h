#pragma once

#include "mpm.h"
#include "multi/mpm_config.h"
#include "multi/multi_enums.h"  // protocol numbers (PROTO_TEST: the test protocol here)

void TEST_init();
uint16_t TEST_callback();

// Futaba FASST (proto_fasst.c): mpm-v2's own, not a MULTI-Module protocol;
// its number is not assigned upstream (the last there: MoFly, 109)
#ifdef FASST_INO
#define PROTO_FASST 110
void FASST_init();
uint16_t FASST_callback();
#endif

// The MULTI-Module protocols (mpm_config.h)
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
#ifdef MJXQ_CCNRF_INO
void MJXQ_init();
uint16_t MJXQ_callback();
#endif
#ifdef CFLIE_NRF24L01_INO
void CFLIE_init();
uint16_t CFLIE_callback();
#endif
#ifdef PROPEL_NRF24L01_INO
void PROPEL_init();
uint16_t PROPEL_callback();
#endif
#ifdef FUTABA_CC2500_INO
void SFHSS_init();
uint16_t SFHSS_callback();
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
#ifdef FRSKY_RX_CC2500_INO
void FRSKY_RX_init();
uint16_t FRSKY_RX_callback();
#endif
#ifdef AFHDS2A_RX_A7105_INO
void AFHDS2A_RX_init();
uint16_t AFHDS2A_RX_callback();
#endif
#ifdef BAYANG_RX_NRF24L01_INO
void BAYANG_RX_init();
uint16_t BAYANG_RX_callback();
#endif
#ifdef DSM_RX_CYRF6936_INO
void DSM_RX_init();
uint16_t DSM_RX_callback();
#endif
