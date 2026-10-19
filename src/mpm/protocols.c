#include "protocols.h"

// clang-format off
const mpm_protocol_t mpm_protocols[] = {
  // id, name, sub protocols (number, name length, names), option, failsafe, channel map, init, callback, end bind
  {PROTO_TEST, "Test", 2, 4, "1M  250k", MPM_OPTION_RFCHAN, false, false, TEST_init, TEST_callback, 0},
};
// clang-format on

const uint8_t mpm_protocol_count = sizeof(mpm_protocols) / sizeof(mpm_protocols[0]);
