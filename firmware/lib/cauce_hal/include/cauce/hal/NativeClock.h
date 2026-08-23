#pragma once

#ifndef ARDUINO

#include <chrono>
#include <cstdint>

#include "cauce/hal/IClock.h"

namespace cauce::hal {

class NativeClock final : public IClock {
 public:
  NativeClock();
  uint32_t monotonicMs() const override;
  uint64_t utcMs() const override;
  bool utcTimeValid() const override;
  void setUtcMs(uint64_t epochMs) override;

 private:
  std::chrono::steady_clock::time_point start_;
};

}  // namespace cauce::hal

#endif
