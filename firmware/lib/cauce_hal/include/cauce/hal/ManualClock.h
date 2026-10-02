#pragma once

#include "cauce/hal/IClock.h"

namespace cauce::hal {

class ManualClock final : public IClock {
 public:
  explicit ManualClock(uint64_t startUtcMs = 0, uint32_t startMonotonicMs = 0)
      : utc_(startUtcMs), mono_(startMonotonicMs), valid_(startUtcMs != 0) {}

  uint32_t monotonicMs() const override { return mono_; }
  uint64_t utcMs() const override { return utc_; }
  bool utcTimeValid() const override { return valid_; }
  void setUtcMs(uint64_t epochMs) override {
    utc_ = epochMs;
    valid_ = true;
  }

  void advanceMs(uint32_t delta) {
    mono_ += delta;
    if (valid_) utc_ += delta;
  }
  // A manual clock advances virtual time rather than blocking.
  void sleepMs(uint32_t durationMs) override { advanceMs(durationMs); }
  void invalidateTime() {
    valid_ = false;
    utc_ = 0;
  }

 private:
  uint64_t utc_;
  uint32_t mono_;
  bool valid_;
};

}  // namespace cauce::hal
