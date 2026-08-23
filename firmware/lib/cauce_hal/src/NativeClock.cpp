#include "cauce/hal/NativeClock.h"

#ifndef ARDUINO

#include <ctime>

namespace cauce::hal {

NativeClock::NativeClock() : start_(std::chrono::steady_clock::now()) {}

uint32_t NativeClock::monotonicMs() const {
  const auto elapsed = std::chrono::steady_clock::now() - start_;
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

uint64_t NativeClock::utcMs() const {
  const auto now = std::chrono::system_clock::now();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())
          .count());
}

bool NativeClock::utcTimeValid() const { return true; }

void NativeClock::setUtcMs(uint64_t epochMs) { (void)epochMs; }

}  // namespace cauce::hal

#endif
