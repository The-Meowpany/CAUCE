#pragma once

#include <cstddef>
#include <cstdint>

namespace cauce::hal {

class IClock {
 public:
  virtual ~IClock() = default;
  virtual uint32_t monotonicMs() const = 0;
  virtual uint64_t utcMs() const = 0;
  virtual bool utcTimeValid() const = 0;
  virtual void setUtcMs(uint64_t epochMs) = 0;

  // Waits between polls. Non-pure on purpose: a test clock advances virtual
  // time instead of blocking, and a caller that has no reason to wait can
  // inherit a no-op rather than implement one.
  virtual void sleepMs(uint32_t durationMs) { (void)durationMs; }
};

}  // namespace cauce::hal
