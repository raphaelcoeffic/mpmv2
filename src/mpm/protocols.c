#include "protocols.h"

// clang-format off
#ifdef FRSKYX_CC2500_INO
#define STR_SUBTYPE_FRSKYX "\x07" "D16\0   " "D16 8ch" "LBT(EU)" "LBT 8ch" "Cloned\0" "Clo 8ch"
#endif

const mpm_protocol_t mpm_protocols[] = {
  // id, name, sub protocol names, number of sub protocols, option, failsafe, channel map, init, callback, end bind
  // (none: bind done). In protocol number order
#ifdef FLYSKY_A7105_INO
  {PROTO_FLYSKY, "FlySky", "\x04" "Std\0" "V9x9" "V6x6" "V912" "CX20", 5, MPM_OPTION_NONE, false, true,
   FLYSKY_init, FLYSKY_callback, 0},
#endif
#ifdef FRSKYD_CC2500_INO
  {PROTO_FRSKYD, "FrSky D", "\x06" "D8\0   " "Cloned", 2, MPM_OPTION_RFTUNE, false, false, FRSKYD_init,
   FRSKYD_callback, 0},
#endif
#ifdef V2X2_NRF24L01_INO
  {PROTO_V2X2, "V2x2", "\x06" "Std\0  " "JXD506" "MR101\0", 3, MPM_OPTION_NONE, false, false, V2X2_init,
   V2X2_callback, end_bind_counter},
#endif
#ifdef DSM_CYRF6936_INO
  {PROTO_DSM, "DSM", "\x04" "2 1F" "2 2F" "X 1F" "X 2F" "Auto" "R 1F" "2SFC", 7, MPM_OPTION_MAXTHR,
   false, true, DSM_init, DSM_callback, 0},
#endif
#ifdef BAYANG_NRF24L01_INO
  {PROTO_BAYANG, "Bayang", "\x07" "Std\0   " "H8S3D\0 " "X16 AH\0" "IRDrone" "DHD D4\0" "QX100\0 ", 6,
   MPM_OPTION_TELEM, false, false, BAYANG_init, BAYANG_callback, end_bind_counter},
#endif
#ifdef FRSKYX_CC2500_INO
  {PROTO_FRSKYX, "FrSky X", STR_SUBTYPE_FRSKYX, 6, MPM_OPTION_RFTUNE, true, false, FRSKYX_init,
   FRSKYX_callback, 0},
#endif
#ifdef FRSKYV_CC2500_INO
  {PROTO_FRSKYV, "FrSky V", 0, 0, MPM_OPTION_RFTUNE, false, false, FRSKYV_init, FRSKYV_callback, 0},
#endif
#ifdef AFHDS2A_A7105_INO
  {PROTO_AFHDS2A, "FlSky2A", "\x08" "PWM,IBUS" "PPM,IBUS" "PWM,SBUS" "PPM,SBUS" "Gyro_Off" "Gyro_On\0" "G_On_Rev", 7,
   MPM_OPTION_SRVFREQ, true, true, AFHDS2A_init, AFHDS2A_callback, 0},
#endif
#ifdef FRSKYX_CC2500_INO
  {PROTO_FRSKYX2, "FrSkyX2", STR_SUBTYPE_FRSKYX, 6, MPM_OPTION_RFTUNE, true, false, FRSKYX_init,
   FRSKYX_callback, 0},
#endif
  {PROTO_TEST, "Test", "\x04" "1M  250k", 2, MPM_OPTION_RFCHAN, false, false, TEST_init, TEST_callback, 0},
};
// clang-format on

const uint8_t mpm_protocol_count = sizeof(mpm_protocols) / sizeof(mpm_protocols[0]);
