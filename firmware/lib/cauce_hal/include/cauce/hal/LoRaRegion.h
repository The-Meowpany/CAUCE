#pragma once

#include <cstring>

#include "cauce/hal/Sx1276Radio.h"

namespace cauce::hal {

// Shared by the node and the gateway bridge: both ends must land on the same frequency, and
// two copies of this table are two chances for them not to.
//
// The band a region implies, and the modulation this project uses everywhere else.
//
// A region name mapped to a frequency here rather than a number in the config file, because a
// *frequency* in a text config is one typo away from putting a node on a band it must not use,
// and there is no safe default for that: 868 MHz and 915 MHz differ enough that a wrong value
// is silent - the radio transmits, and nobody nearby hears it.
//
// Returns false for a region this build does not know, and the caller reports it rather than
// falling back. Falling back to 868 MHz for an unrecognised region would put an AU node on the
// European band, which is the exact failure this refuses.
inline bool loraDefaultsFor(const char* region, Sx1276Config& out) {
  out = Sx1276Config{};
  if (region == nullptr || region[0] == '\0') return false;
  if (std::strcmp(region, "EU868") == 0 || std::strcmp(region, "EU433") == 0) {
    out.frequencyHz = 868100000u;  // EU868 only; EU433 is 434 and is not supported here
    return std::strcmp(region, "EU868") == 0;
  }
  if (std::strcmp(region, "US915") == 0) {
    out.frequencyHz = 915000000u;
    return true;
  }
  if (std::strcmp(region, "AU915") == 0) {
    out.frequencyHz = 915200000u;
    return true;
  }
  if (std::strcmp(region, "AS923") == 0) {
    out.frequencyHz = 923200000u;
    return true;
  }
  return false;
}

}  // namespace cauce::hal
