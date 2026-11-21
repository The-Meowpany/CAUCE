#pragma once

#include <cstdint>

#include "cauce/hal/II2cBus.h"

namespace cauce::drivers {

// DS3231 temperature-compensated RTC (+/-2 ppm). Not a sensor: it is the clock source that sits
// between NTP and the free-running internal oscillator, so a node that boots without network
// still stamps its records with a trustworthy time instead of marking them TIME_UNCERTAIN.
//
// The chip keeps UTC; time zones are a presentation concern on the central.
class Ds3231Rtc {
 public:
  static constexpr uint8_t kAddress = 0x68;

  explicit Ds3231Rtc(hal::II2cBus& bus);

  bool present();
  // False when the chip reports its oscillator stopped (battery removed or flat), in which case
  // its time is not to be trusted even though it reads back without error.
  bool timeTrustworthy();
  bool readUtcMs(uint64_t& outEpochMs);
  // Writes the time and clears the oscillator-stop flag.
  bool writeUtcMs(uint64_t epochMs);

  // Calendar conversions, exposed for tests. Valid for 2000..2099.
  static uint64_t toEpochMs(int year, int month, int day, int hour, int minute, int second);
  static void fromEpochMs(uint64_t epochMs, int& year, int& month, int& day, int& hour,
                          int& minute, int& second);

 private:
  hal::II2cBus& bus_;
};

}  // namespace cauce::drivers
