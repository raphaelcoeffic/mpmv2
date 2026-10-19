#include "protocols.h"

// clang-format off
const mpm_protocol_t mpm_protocols[] = {
  // id, name, sub protocol names, number of sub protocols, option, failsafe, channel map, init, callback, end bind
#ifdef BAYANG_NRF24L01_INO
  {PROTO_BAYANG, "Bayang", "\x07" "Std\0   " "H8S3D\0 " "X16 AH\0" "IRDrone" "DHD D4\0" "QX100\0 ", 6,
   MPM_OPTION_TELEM, false, false, BAYANG_init, BAYANG_callback, end_bind_counter},
#endif
  {PROTO_TEST, "Test", "\x04" "1M  250k", 2, MPM_OPTION_RFCHAN, false, false, TEST_init, TEST_callback, 0},
};
// clang-format on

const uint8_t mpm_protocol_count = sizeof(mpm_protocols) / sizeof(mpm_protocols[0]);
